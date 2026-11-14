/*
 * clawt-egress-gate.c - The one way out of a gated agent's sandbox
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "net/clawt-egress-gate.h"
#include "clawt-net.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>

/* How many refusals each agent keeps for the clients to show. */
#define RECENT_REFUSALS 20

/* A per-address connect timeout, before the next address is tried. */
#define CONNECT_TIMEOUT_SECONDS 10

/*
 * Connections in flight per agent.  A runaway loop opening tunnels is
 * answered with 503 here rather than by the daemon running out of file
 * descriptors for everybody.
 */
#define MAX_CONNECTIONS 256

/* ── Requests ────────────────────────────────────────────────────── */

void
clawt_egress_request_free(
	ClawtEgressRequest *request
){
	if (request == NULL)
		return;

	g_free(request->host);
	g_free(request->forward_head);
	g_free(request);
}

gssize
clawt_egress_find_head_end(
	const guint8 *data,
	gsize         length
){
	gsize i;

	for (i = 0; i + 3 < length; i++)
		if (data[i] == '\r' && data[i + 1] == '\n' &&
		    data[i + 2] == '\r' && data[i + 3] == '\n')
			return (gssize)(i + 4);

	return -1;
}

/*
 * A host as a proxy may be asked for it: an address literal, or a name
 * made of the characters names are made of.  Anything else -- a space,
 * a slash, a percent sign, an at sign -- is somebody trying to make the
 * resolver or the log say something it should not.
 */
static gboolean
valid_host(
	const gchar *host
){
	const gchar *p;
	gsize length;

	if (host == NULL || *host == '\0')
		return FALSE;

	if (g_hostname_is_ip_address(host))
		return TRUE;

	length = strlen(host);

	if (length > 253)
		return FALSE;

	for (p = host; *p != '\0'; p++)
		if (!g_ascii_isalnum(*p) && *p != '-' && *p != '.' && *p != '_')
			return FALSE;

	return host[0] != '.' && host[0] != '-';
}

/*
 * host:port or [v6]:port, as a CONNECT target or a URL authority.  The
 * bracket form is the only one an IPv6 literal can take, because its
 * own colons cannot be told from the port's.
 */
static gboolean
split_authority(
	const gchar  *authority,
	guint16       default_port,
	gchar       **out_host,
	guint16      *out_port
){
	g_autofree gchar *host = NULL;
	const gchar *port_text = NULL;
	guint64 port = 0;

	if (authority[0] == '[') {
		const gchar *close = strchr(authority, ']');

		if (close == NULL)
			return FALSE;

		host = g_strndup(authority + 1, (gsize)(close - authority - 1));

		if (!g_hostname_is_ip_address(host) || strchr(host, ':') == NULL)
			return FALSE;

		if (close[1] == ':')
			port_text = close + 2;
		else if (close[1] != '\0')
			return FALSE;
	} else {
		const gchar *colon = strrchr(authority, ':');

		if (colon != NULL && strchr(authority, ':') != colon)
			return FALSE;

		if (colon != NULL) {
			host = g_strndup(authority, (gsize)(colon - authority));
			port_text = colon + 1;
		} else {
			host = g_strdup(authority);
		}
	}

	if (port_text != NULL) {
		if (!g_ascii_string_to_unsigned(port_text, 10, 1, 65535, &port,
		                                NULL))
			return FALSE;
	} else if (default_port != 0) {
		port = default_port;
	} else {
		return FALSE;
	}

	if (!valid_host(host))
		return FALSE;

	*out_host = g_steal_pointer(&host);
	*out_port = (guint16)port;
	return TRUE;
}

static gboolean
header_is_dropped(
	const gchar *line
){
	static const gchar *const dropped[] = {
		"proxy-connection", "proxy-authorization", "connection",
		"keep-alive", "te", "trailer", "upgrade", NULL
	};
	const gchar *colon = strchr(line, ':');
	g_autofree gchar *name = NULL;
	guint i;

	if (colon == NULL)
		return TRUE;

	name = g_ascii_strdown(line, (gssize)(colon - line));
	g_strstrip(name);

	for (i = 0; dropped[i] != NULL; i++)
		if (g_strcmp0(name, dropped[i]) == 0)
			return TRUE;

	return FALSE;
}

ClawtEgressRequest *
clawt_egress_parse_request(
	const gchar  *head,
	gsize         length,
	GError      **error
){
	g_autoptr(ClawtEgressRequest) request = NULL;
	g_autofree gchar *text = NULL;
	g_auto(GStrv) lines = NULL;
	g_auto(GStrv) words = NULL;
	const gchar *method;
	const gchar *target;

	g_return_val_if_fail(head != NULL, NULL);

	/* A NUL inside the head is not HTTP, and would truncate the parse. */
	if (memchr(head, '\0', length) != NULL) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
		                    "the request contains a NUL byte");
		return NULL;
	}

	text = g_strndup(head, length);
	lines = g_strsplit(text, "\r\n", -1);

	if (lines[0] == NULL || *lines[0] == '\0') {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
		                    "the request has no request line");
		return NULL;
	}

	words = g_strsplit(lines[0], " ", -1);

	if (g_strv_length(words) != 3 || !g_str_has_prefix(words[2], "HTTP/1.")) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
		                    "the request line is not METHOD TARGET HTTP/1.x");
		return NULL;
	}

	method = words[0];
	target = words[1];

	request = g_new0(ClawtEgressRequest, 1);

	if (g_ascii_strcasecmp(method, "CONNECT") == 0) {
		if (!split_authority(target, 0, &request->host, &request->port)) {
			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
			            "CONNECT needs host:port or [address]:port, not "
			            "'%s'", target);
			return NULL;
		}

		request->is_connect = TRUE;
		return g_steal_pointer(&request);
	}

	if (g_ascii_strncasecmp(target, "https://", 8) == 0) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
		                    "an https:// URL goes through the gate as a "
		                    "CONNECT tunnel, not as a plain request");
		return NULL;
	}

	if (g_ascii_strncasecmp(target, "http://", 7) != 0) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
		                    "this is clawtilla's egress gate, an HTTP proxy: "
		                    "send CONNECT host:port, or a request with an "
		                    "absolute http:// URL");
		return NULL;
	}

	{
		const gchar *authority = target + 7;
		const gchar *path = strchr(authority, '/');
		g_autofree gchar *host_part = NULL;
		GString *forward;
		guint i;

		host_part = (path != NULL)
		            ? g_strndup(authority, (gsize)(path - authority))
		            : g_strdup(authority);

		/*
		 * Userinfo in a URL is a credential and a classic way to make a
		 * log line lie about the host ("http://good.example@evil/").
		 */
		if (strchr(host_part, '@') != NULL ||
		    !split_authority(host_part, 80, &request->host,
		                     &request->port)) {
			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
			            "'%s' does not name a host the gate can reach",
			            host_part);
			return NULL;
		}

		forward = g_string_new(NULL);
		g_string_append_printf(forward, "%s %s %s\r\n", method,
		                       path != NULL ? path : "/", words[2]);

		for (i = 1; lines[i] != NULL && *lines[i] != '\0'; i++) {
			if (header_is_dropped(lines[i]))
				continue;

			g_string_append(forward, lines[i]);
			g_string_append(forward, "\r\n");
		}

		g_string_append(forward, "Connection: close\r\n\r\n");
		request->forward_head = g_string_free(forward, FALSE);
	}

	return g_steal_pointer(&request);
}

/* ── Refusals ────────────────────────────────────────────────────── */

ClawtEgressRefusal *
clawt_egress_refusal_copy(
	const ClawtEgressRefusal *refusal
){
	ClawtEgressRefusal *copy;

	g_return_val_if_fail(refusal != NULL, NULL);

	copy = g_new0(ClawtEgressRefusal, 1);
	copy->at = refusal->at;
	copy->host = g_strdup(refusal->host);
	copy->port = refusal->port;
	copy->address = g_strdup(refusal->address);
	copy->reason = g_strdup(refusal->reason);

	return copy;
}

void
clawt_egress_refusal_free(
	ClawtEgressRefusal *refusal
){
	if (refusal == NULL)
		return;

	g_free(refusal->host);
	g_free(refusal->address);
	g_free(refusal->reason);
	g_free(refusal);
}

G_DEFINE_BOXED_TYPE(ClawtEgressRefusal, clawt_egress_refusal,
                    clawt_egress_refusal_copy, clawt_egress_refusal_free)

/* ── The gate ────────────────────────────────────────────────────── */

/*
 * One agent's socket, and what it has been refused.  Kept after the
 * socket closes, so a stopped agent's refusals are still there to read.
 */
typedef struct {
	ClawtEgressGate  *gate;            /* weak: the gate owns this */
	gchar            *agent_id;
	gchar            *path;
	GSocketService   *service;
	GQueue           *refusals;        /* ClawtEgressRefusal, newest first */
	guint             refused_today;
	gint              refused_day;     /* local day the count is for */
	guint             connections;
} AgentGate;

struct _ClawtEgressGate {
	GObject               parent_instance;

	GMainContext         *context;
	gchar                *socket_dir;
	ClawtEgressAllowlist *allow;
	GHashTable           *agents;      /* agent id -> AgentGate */
	GCancellable         *cancellable;
};

enum {
	SIGNAL_REFUSED,
	N_SIGNALS
};

static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(ClawtEgressGate, clawt_egress_gate, G_TYPE_OBJECT)

static void
agent_gate_stop_service(
	AgentGate *agent
){
	if (agent->service == NULL)
		return;

	g_socket_service_stop(agent->service);
	g_socket_listener_close(G_SOCKET_LISTENER(agent->service));
	g_clear_object(&agent->service);

	if (agent->path != NULL)
		g_unlink(agent->path);
}

static void
agent_gate_free(
	gpointer data
){
	AgentGate *agent = data;

	agent_gate_stop_service(agent);
	g_free(agent->agent_id);
	g_free(agent->path);
	g_queue_free_full(agent->refusals,
	                  (GDestroyNotify)clawt_egress_refusal_free);
	g_free(agent);
}

static AgentGate *
agent_gate_get(
	ClawtEgressGate *self,
	const gchar     *agent_id
){
	AgentGate *agent = g_hash_table_lookup(self->agents, agent_id);

	if (agent != NULL)
		return agent;

	agent = g_new0(AgentGate, 1);
	agent->gate = self;
	agent->agent_id = g_strdup(agent_id);
	agent->refusals = g_queue_new();
	agent->refused_day = -1;
	g_hash_table_insert(self->agents, g_strdup(agent_id), agent);

	return agent;
}

/* The local calendar day, as one comparable number. */
static gint
today(void)
{
	g_autoptr(GDateTime) now = g_date_time_new_now_local();

	return g_date_time_get_year(now) * 1000 +
	       g_date_time_get_day_of_year(now);
}

static void
clawt_egress_gate_dispose(
	GObject *object
){
	ClawtEgressGate *self = CLAWT_EGRESS_GATE(object);

	clawt_egress_gate_stop(self);

	G_OBJECT_CLASS(clawt_egress_gate_parent_class)->dispose(object);
}

static void
clawt_egress_gate_finalize(
	GObject *object
){
	ClawtEgressGate *self = CLAWT_EGRESS_GATE(object);

	g_clear_pointer(&self->agents, g_hash_table_unref);
	g_clear_pointer(&self->allow, clawt_egress_allowlist_unref);
	g_clear_pointer(&self->context, g_main_context_unref);
	g_clear_object(&self->cancellable);
	g_free(self->socket_dir);

	G_OBJECT_CLASS(clawt_egress_gate_parent_class)->finalize(object);
}

static void
clawt_egress_gate_class_init(
	ClawtEgressGateClass *klass
){
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->dispose = clawt_egress_gate_dispose;
	object_class->finalize = clawt_egress_gate_finalize;

	/**
	 * ClawtEgressGate::refused:
	 * @gate: the gate
	 * @agent_id: whose connection it was
	 * @refusal: what was refused and why
	 *
	 * Emitted for every connection the gate refuses.
	 */
	signals[SIGNAL_REFUSED] = g_signal_new(
		"refused", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
		NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_STRING,
		CLAWT_TYPE_EGRESS_REFUSAL);
}

static void
clawt_egress_gate_init(
	ClawtEgressGate *self
){
	self->agents = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                     agent_gate_free);
	self->cancellable = g_cancellable_new();
}

ClawtEgressGate *
clawt_egress_gate_new(
	GMainContext *context,
	const gchar  *socket_dir
){
	ClawtEgressGate *self;

	g_return_val_if_fail(socket_dir != NULL, NULL);

	self = g_object_new(CLAWT_TYPE_EGRESS_GATE, NULL);
	self->socket_dir = g_strdup(socket_dir);

	/*
	 * Captured here, where the sockets will be attached, rather than
	 * asked for at each async call: dispatching a source does not push
	 * its context, so the thread-default inside a callback is whatever
	 * the dispatcher had -- for an embedded daemon, a loop nobody runs.
	 */
	self->context = (context != NULL)
	                ? g_main_context_ref(context)
	                : g_main_context_ref_thread_default();

	return self;
}

void
clawt_egress_gate_set_allowlist(
	ClawtEgressGate      *self,
	ClawtEgressAllowlist *allow
){
	g_return_if_fail(CLAWT_IS_EGRESS_GATE(self));

	g_clear_pointer(&self->allow, clawt_egress_allowlist_unref);

	if (allow != NULL)
		self->allow = clawt_egress_allowlist_ref(allow);
}

ClawtEgressAllowlist *
clawt_egress_gate_get_allowlist(
	ClawtEgressGate *self
){
	g_return_val_if_fail(CLAWT_IS_EGRESS_GATE(self), NULL);

	return self->allow;
}

gchar *
clawt_egress_gate_socket_path(
	ClawtEgressGate *self,
	const gchar     *agent_id
){
	g_autofree gchar *name = NULL;

	g_return_val_if_fail(CLAWT_IS_EGRESS_GATE(self), NULL);
	g_return_val_if_fail(agent_id != NULL, NULL);

	name = g_strconcat(agent_id, ".sock", NULL);

	return g_build_filename(self->socket_dir, name, NULL);
}

void
clawt_egress_gate_record_refusal(
	ClawtEgressGate *self,
	const gchar     *agent_id,
	const gchar     *host,
	guint16          port,
	const gchar     *address,
	const gchar     *reason
){
	AgentGate *agent;
	ClawtEgressRefusal *refusal;
	gint day = today();

	g_return_if_fail(CLAWT_IS_EGRESS_GATE(self));
	g_return_if_fail(agent_id != NULL);

	agent = agent_gate_get(self, agent_id);

	if (agent->refused_day != day) {
		agent->refused_day = day;
		agent->refused_today = 0;
	}

	agent->refused_today++;

	refusal = g_new0(ClawtEgressRefusal, 1);
	refusal->at = g_get_real_time();
	refusal->host = g_strdup(host);
	refusal->port = port;
	refusal->address = g_strdup(address);
	refusal->reason = g_strdup(reason);

	g_queue_push_head(agent->refusals, refusal);

	while (g_queue_get_length(agent->refusals) > RECENT_REFUSALS)
		clawt_egress_refusal_free(g_queue_pop_tail(agent->refusals));

	g_signal_emit(self, signals[SIGNAL_REFUSED], 0, agent_id, refusal);
}

guint
clawt_egress_gate_refused_today(
	ClawtEgressGate *self,
	const gchar     *agent_id
){
	AgentGate *agent;

	g_return_val_if_fail(CLAWT_IS_EGRESS_GATE(self), 0);

	agent = (agent_id != NULL) ? g_hash_table_lookup(self->agents, agent_id)
	                           : NULL;

	if (agent == NULL || agent->refused_day != today())
		return 0;

	return agent->refused_today;
}

GPtrArray *
clawt_egress_gate_recent_refusals(
	ClawtEgressGate *self,
	const gchar     *agent_id
){
	GPtrArray *out;
	AgentGate *agent;
	GList *l;

	g_return_val_if_fail(CLAWT_IS_EGRESS_GATE(self), NULL);

	out = g_ptr_array_new_with_free_func(
		(GDestroyNotify)clawt_egress_refusal_free);
	agent = (agent_id != NULL) ? g_hash_table_lookup(self->agents, agent_id)
	                           : NULL;

	if (agent == NULL)
		return out;

	for (l = agent->refusals->head; l != NULL; l = l->next)
		g_ptr_array_add(out, clawt_egress_refusal_copy(l->data));

	return out;
}

/* ── One connection ──────────────────────────────────────────────── */

typedef struct {
	ClawtEgressGate    *gate;
	gchar              *agent_id;
	GSocketConnection  *client;
	GCancellable       *cancellable;
	GByteArray         *buffer;
	gsize               head_length;
	ClawtEgressRequest *request;
	GPtrArray          *addresses;   /* GInetAddress, each one checked */
	guint               next;
	GSocketClient      *connector;
	GSocketConnection  *upstream;
	gchar              *last_error;
	gboolean            counted;
} Conn;

static void
conn_free(
	Conn *conn
){
	if (conn->counted) {
		AgentGate *agent = g_hash_table_lookup(conn->gate->agents,
		                                       conn->agent_id);

		if (agent != NULL && agent->connections > 0)
			agent->connections--;
	}

	g_clear_object(&conn->upstream);
	g_clear_object(&conn->connector);
	g_clear_pointer(&conn->addresses, g_ptr_array_unref);
	g_clear_pointer(&conn->request, clawt_egress_request_free);
	g_clear_pointer(&conn->buffer, g_byte_array_unref);
	g_clear_object(&conn->cancellable);
	g_clear_object(&conn->client);
	g_object_unref(conn->gate);
	g_free(conn->agent_id);
	g_free(conn->last_error);
	g_free(conn);
}

/*
 * Every async call below names the gate's context by pushing it: the
 * *_async family attaches to whatever is thread-default, and inside a
 * callback that is not necessarily the context the callback came from.
 */
#define WITH_CONTEXT(gate, call) \
	G_STMT_START { \
		g_main_context_push_thread_default((gate)->context); \
		call; \
		g_main_context_pop_thread_default((gate)->context); \
	} G_STMT_END

static void
on_reply_written(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	Conn *conn = user_data;

	g_output_stream_write_all_finish(G_OUTPUT_STREAM(source), result, NULL,
	                                 NULL);
	g_io_stream_close(G_IO_STREAM(conn->client), NULL, NULL);
	conn_free(conn);
}

/*
 * An answer that ends the connection.  The reason goes in the body and a
 * header, because the client is a CLI that will print one of them, and
 * "403" with nothing after it reads as the remote site refusing.
 */
static void
conn_answer(
	Conn        *conn,
	guint        status,
	const gchar *phrase,
	const gchar *reason
){
	g_autofree gchar *body = NULL;
	g_autofree gchar *flat = NULL;
	GOutputStream *out;
	gchar *reply;

	flat = g_strdup(reason != NULL ? reason : phrase);
	g_strdelimit(flat, "\r\n", ' ');

	body = g_strdup_printf("clawtilla egress gate: %s\n", flat);
	reply = g_strdup_printf("HTTP/1.1 %u %s\r\n"
	                        "Content-Type: text/plain; charset=utf-8\r\n"
	                        "X-Clawtilla-Egress: %s\r\n"
	                        "Content-Length: %" G_GSIZE_FORMAT "\r\n"
	                        "Connection: close\r\n\r\n%s",
	                        status, phrase, flat, strlen(body), body);

	out = g_io_stream_get_output_stream(G_IO_STREAM(conn->client));

	/* The reply is owned by the closure; freed when the write is done. */
	g_object_set_data_full(G_OBJECT(conn->client), "clawt-egress-reply",
	                       reply, g_free);

	WITH_CONTEXT(conn->gate,
		g_output_stream_write_all_async(out, reply, strlen(reply),
		                                G_PRIORITY_DEFAULT, NULL,
		                                on_reply_written, conn));
}

static void
conn_refuse(
	Conn        *conn,
	const gchar *address,
	const gchar *reason
){
	clawt_egress_gate_record_refusal(conn->gate, conn->agent_id,
	                                 conn->request->host,
	                                 conn->request->port, address, reason);
	conn_answer(conn, 403, "Forbidden", reason);
}

static void
on_spliced(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	Conn *conn = user_data;

	g_io_stream_splice_finish(result, NULL);
	conn_free(conn);
}

static void
conn_splice(
	Conn *conn
){
	WITH_CONTEXT(conn->gate,
		g_io_stream_splice_async(G_IO_STREAM(conn->client),
		                         G_IO_STREAM(conn->upstream),
		                         G_IO_STREAM_SPLICE_CLOSE_STREAM1 |
		                         G_IO_STREAM_SPLICE_CLOSE_STREAM2 |
		                         G_IO_STREAM_SPLICE_WAIT_FOR_BOTH,
		                         G_PRIORITY_DEFAULT, conn->cancellable,
		                         on_spliced, conn));
}

/*
 * What arrived after the head: for a CONNECT, the start of the TLS
 * handshake a client sent without waiting; for plain HTTP, the start of
 * the body.  It belongs upstream either way.
 */
static GBytes *
conn_leftover(
	Conn *conn
){
	return g_bytes_new(conn->buffer->data + conn->head_length,
	                   conn->buffer->len - conn->head_length);
}

static void
on_upstream_written(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	Conn *conn = user_data;

	if (!g_output_stream_write_all_finish(G_OUTPUT_STREAM(source), result,
	                                      NULL, NULL)) {
		conn_free(conn);
		return;
	}

	conn_splice(conn);
}

static void
on_established_written(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	Conn *conn = user_data;
	g_autoptr(GBytes) leftover = NULL;
	GOutputStream *up;

	if (!g_output_stream_write_all_finish(G_OUTPUT_STREAM(source), result,
	                                      NULL, NULL)) {
		conn_free(conn);
		return;
	}

	leftover = conn_leftover(conn);

	if (g_bytes_get_size(leftover) == 0) {
		conn_splice(conn);
		return;
	}

	up = g_io_stream_get_output_stream(G_IO_STREAM(conn->upstream));
	g_object_set_data_full(G_OBJECT(conn->upstream), "clawt-egress-head",
	                       g_bytes_ref(leftover),
	                       (GDestroyNotify)g_bytes_unref);

	WITH_CONTEXT(conn->gate,
		g_output_stream_write_all_async(
			up, g_bytes_get_data(leftover, NULL),
			g_bytes_get_size(leftover), G_PRIORITY_DEFAULT,
			conn->cancellable, on_upstream_written, conn));
}

static void conn_connect_next(Conn *conn);

/*
 * Connected.  The address the socket actually reached is read back and
 * judged again, rather than assumed to be the one asked for: the check
 * is about where the bytes go, and this is the only place that knows.
 */
static void
conn_established(
	Conn *conn
){
	g_autoptr(GSocketAddress) remote = NULL;
	g_autoptr(GPtrArray) reached = NULL;
	g_autofree gchar *refusal = NULL;
	g_autofree gchar *text = NULL;
	GInetAddress *address;

	remote = g_socket_connection_get_remote_address(conn->upstream, NULL);

	if (remote == NULL || !G_IS_INET_SOCKET_ADDRESS(remote)) {
		conn_answer(conn, 502, "Bad Gateway",
		            "could not tell which address the connection reached");
		return;
	}

	address = g_inet_socket_address_get_address(
		G_INET_SOCKET_ADDRESS(remote));
	reached = g_ptr_array_new_with_free_func(g_object_unref);
	g_ptr_array_add(reached, g_object_ref(address));

	refusal = clawt_egress_decide(conn->request->host, conn->request->port,
	                              reached,
	                              clawt_egress_gate_get_allowlist(conn->gate));

	if (refusal != NULL) {
		text = g_inet_address_to_string(address);
		g_io_stream_close(G_IO_STREAM(conn->upstream), NULL, NULL);
		conn_refuse(conn, text, refusal);
		return;
	}

	if (conn->request->is_connect) {
		static const gchar established[] =
			"HTTP/1.1 200 Connection Established\r\n\r\n";
		GOutputStream *out =
			g_io_stream_get_output_stream(G_IO_STREAM(conn->client));

		WITH_CONTEXT(conn->gate,
			g_output_stream_write_all_async(
				out, established, sizeof(established) - 1,
				G_PRIORITY_DEFAULT, conn->cancellable,
				on_established_written, conn));
		return;
	}

	/*
	 * Plain HTTP: the rewritten head, then whatever of the body came
	 * with it, then the two streams joined.
	 */
	{
		g_autoptr(GBytes) leftover = conn_leftover(conn);
		GByteArray *first = g_byte_array_new();
		GOutputStream *up =
			g_io_stream_get_output_stream(G_IO_STREAM(conn->upstream));
		GBytes *bytes;

		g_byte_array_append(first, (const guint8 *)conn->request->forward_head,
		                    (guint)strlen(conn->request->forward_head));
		g_byte_array_append(first, g_bytes_get_data(leftover, NULL),
		                    (guint)g_bytes_get_size(leftover));
		bytes = g_byte_array_free_to_bytes(first);

		g_object_set_data_full(G_OBJECT(conn->upstream), "clawt-egress-head",
		                       bytes, (GDestroyNotify)g_bytes_unref);

		WITH_CONTEXT(conn->gate,
			g_output_stream_write_all_async(
				up, g_bytes_get_data(bytes, NULL),
				g_bytes_get_size(bytes), G_PRIORITY_DEFAULT,
				conn->cancellable, on_upstream_written, conn));
	}
}

static void
on_connected(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	Conn *conn = user_data;
	g_autoptr(GError) error = NULL;

	conn->upstream = g_socket_client_connect_finish(G_SOCKET_CLIENT(source),
	                                                result, &error);

	if (conn->upstream == NULL) {
		if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
			conn_free(conn);
			return;
		}

		/*
		 * The next address, not a failure: one family not routing is
		 * the ordinary case on a host with an AAAA answer and no IPv6.
		 */
		g_free(conn->last_error);
		conn->last_error = g_strdup(error->message);
		conn_connect_next(conn);
		return;
	}

	conn_established(conn);
}

static void
conn_connect_next(
	Conn *conn
){
	g_autoptr(GSocketAddress) target = NULL;
	GInetAddress *address;

	if (conn->next >= conn->addresses->len) {
		g_autofree gchar *why = g_strdup_printf(
			"could not connect to %s port %u: %s", conn->request->host,
			conn->request->port,
			conn->last_error != NULL ? conn->last_error : "no address");

		conn_answer(conn, 502, "Bad Gateway", why);
		return;
	}

	address = g_ptr_array_index(conn->addresses, conn->next++);
	target = g_inet_socket_address_new(address, conn->request->port);

	if (conn->connector == NULL) {
		conn->connector = g_socket_client_new();

		/*
		 * Never through a proxy of the daemon's: the address handed in
		 * is the checked one, and a proxy would connect wherever it
		 * liked.  TLS is the client's business inside the tunnel.
		 */
		g_socket_client_set_enable_proxy(conn->connector, FALSE);
		g_socket_client_set_timeout(conn->connector,
		                            CONNECT_TIMEOUT_SECONDS);
	}

	WITH_CONTEXT(conn->gate,
		g_socket_client_connect_async(conn->connector,
		                              G_SOCKET_CONNECTABLE(target),
		                              conn->cancellable, on_connected,
		                              conn));
}

/*
 * Resolved: every address is judged before any is tried.  Interleaving
 * the families means a host whose first family cannot route reaches the
 * other one after a single failure rather than after all of its own.
 */
static void
conn_decide(
	Conn *conn
){
	g_autofree gchar *refusal = NULL;
	g_autoptr(GPtrArray) ordered = NULL;
	guint v4 = 0;
	guint v6 = 0;
	guint i;

	refusal = clawt_egress_decide(conn->request->host, conn->request->port,
	                              conn->addresses,
	                              clawt_egress_gate_get_allowlist(conn->gate));

	if (refusal != NULL) {
		g_autofree gchar *refused = NULL;

		/* The address that was refused, for the clients to show. */
		for (i = 0; i < conn->addresses->len && refused == NULL; i++) {
			GInetAddress *a = g_ptr_array_index(conn->addresses, i);

			if (!clawt_net_address_is_public(a) &&
			    !clawt_egress_allowlist_permits(
			        clawt_egress_gate_get_allowlist(conn->gate),
			        conn->request->host, a, conn->request->port))
				refused = g_inet_address_to_string(a);
		}

		conn_refuse(conn, refused, refusal);
		return;
	}

	ordered = g_ptr_array_new_with_free_func(g_object_unref);

	while (ordered->len < conn->addresses->len) {
		gboolean added = FALSE;

		for (i = v6; i < conn->addresses->len; i++) {
			GInetAddress *a = g_ptr_array_index(conn->addresses, i);

			v6 = i + 1;
			if (g_inet_address_get_family(a) == G_SOCKET_FAMILY_IPV6) {
				g_ptr_array_add(ordered, g_object_ref(a));
				added = TRUE;
				break;
			}
		}

		for (i = v4; i < conn->addresses->len; i++) {
			GInetAddress *a = g_ptr_array_index(conn->addresses, i);

			v4 = i + 1;
			if (g_inet_address_get_family(a) == G_SOCKET_FAMILY_IPV4) {
				g_ptr_array_add(ordered, g_object_ref(a));
				added = TRUE;
				break;
			}
		}

		if (!added)
			break;
	}

	g_ptr_array_unref(conn->addresses);
	conn->addresses = g_steal_pointer(&ordered);
	conn->next = 0;
	conn_connect_next(conn);
}

static void
on_resolved(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	Conn *conn = user_data;
	g_autoptr(GError) error = NULL;
	GList *found;
	GList *l;

	found = g_resolver_lookup_by_name_finish(G_RESOLVER(source), result,
	                                         &error);

	if (found == NULL) {
		g_autofree gchar *why = NULL;

		if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
			conn_free(conn);
			return;
		}

		why = g_strdup_printf("could not resolve %s: %s",
		                      conn->request->host,
		                      error != NULL ? error->message : "no answer");
		conn_answer(conn, 502, "Bad Gateway", why);
		return;
	}

	conn->addresses = g_ptr_array_new_with_free_func(g_object_unref);

	for (l = found; l != NULL; l = l->next)
		g_ptr_array_add(conn->addresses, g_object_ref(l->data));

	g_resolver_free_addresses(found);
	conn_decide(conn);
}

static void
conn_resolve(
	Conn *conn
){
	g_autoptr(GInetAddress) literal = NULL;

	literal = g_inet_address_new_from_string(conn->request->host);

	if (literal != NULL) {
		conn->addresses = g_ptr_array_new_with_free_func(g_object_unref);
		g_ptr_array_add(conn->addresses, g_steal_pointer(&literal));
		conn_decide(conn);
		return;
	}

	{
		g_autoptr(GResolver) resolver = g_resolver_get_default();

		WITH_CONTEXT(conn->gate,
			g_resolver_lookup_by_name_async(resolver, conn->request->host,
			                                conn->cancellable, on_resolved,
			                                conn));
	}
}

static void conn_read(Conn *conn);

static void
on_read(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	Conn *conn = user_data;
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(GError) error = NULL;
	gssize end;

	bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(source), result,
	                                         &error);

	if (bytes == NULL || g_bytes_get_size(bytes) == 0) {
		conn_free(conn);
		return;
	}

	g_byte_array_append(conn->buffer, g_bytes_get_data(bytes, NULL),
	                    (guint)g_bytes_get_size(bytes));

	end = clawt_egress_find_head_end(conn->buffer->data, conn->buffer->len);

	if (end < 0) {
		if (conn->buffer->len >= CLAWT_EGRESS_MAX_HEAD) {
			conn_answer(conn, 431, "Request Header Fields Too Large",
			            "the request head is longer than the gate reads");
			return;
		}

		conn_read(conn);
		return;
	}

	conn->head_length = (gsize)end;

	{
		g_autoptr(GError) parse_error = NULL;

		conn->request = clawt_egress_parse_request(
			(const gchar *)conn->buffer->data, conn->head_length,
			&parse_error);

		if (conn->request == NULL) {
			conn_answer(conn, 400, "Bad Request", parse_error->message);
			return;
		}
	}

	conn_resolve(conn);
}

static void
conn_read(
	Conn *conn
){
	GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(conn->client));
	gsize room = CLAWT_EGRESS_MAX_HEAD - conn->buffer->len;

	/* Never more than the head may be, so the bound is on what is read. */
	WITH_CONTEXT(conn->gate,
		g_input_stream_read_bytes_async(in, MIN(room, (gsize)4096),
		                                G_PRIORITY_DEFAULT,
		                                conn->cancellable, on_read, conn));
}

static gboolean
on_incoming(
	GSocketService    *service,
	GSocketConnection *connection,
	GObject           *source_object,
	gpointer           user_data
){
	AgentGate *agent = user_data;
	ClawtEgressGate *self = agent->gate;
	Conn *conn;

	conn = g_new0(Conn, 1);
	conn->gate = g_object_ref(self);
	conn->agent_id = g_strdup(agent->agent_id);
	conn->client = g_object_ref(connection);
	conn->cancellable = g_cancellable_new();
	conn->buffer = g_byte_array_new();

	/* One parent cancellable, so stop() reaches every connection. */
	g_signal_connect_object(self->cancellable, "cancelled",
	                        G_CALLBACK(g_cancellable_cancel),
	                        conn->cancellable, G_CONNECT_SWAPPED);

	if (agent->connections >= MAX_CONNECTIONS) {
		g_autofree gchar *why = g_strdup_printf(
			"this agent already has %u connections open through the gate",
			agent->connections);

		conn_answer(conn, 503, "Service Unavailable", why);
		return TRUE;
	}

	agent->connections++;
	conn->counted = TRUE;
	conn_read(conn);

	return TRUE;
}

gchar *
clawt_egress_gate_open(
	ClawtEgressGate  *self,
	const gchar      *agent_id,
	GError          **error
){
	g_autoptr(GSocketAddress) address = NULL;
	g_autoptr(GSocketService) service = NULL;
	AgentGate *agent;
	gboolean bound;

	g_return_val_if_fail(CLAWT_IS_EGRESS_GATE(self), NULL);
	g_return_val_if_fail(agent_id != NULL, NULL);

	agent = agent_gate_get(self, agent_id);

	if (agent->service != NULL && agent->path != NULL &&
	    g_file_test(agent->path, G_FILE_TEST_EXISTS))
		return g_strdup(agent->path);

	agent_gate_stop_service(agent);
	g_free(agent->path);
	agent->path = clawt_egress_gate_socket_path(self, agent_id);

	if (!clawt_check_socket_path(agent->path, error))
		return NULL;

	if (g_mkdir_with_parents(self->socket_dir, 0700) != 0) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
		            "could not create %s: %s", self->socket_dir,
		            g_strerror(errno));
		return NULL;
	}

	/*
	 * A socket left by an earlier daemon.  Ours to remove: the daemon
	 * holds the state directory's lock, so nothing else is serving it.
	 */
	g_unlink(agent->path);

	address = g_unix_socket_address_new(agent->path);
	service = g_socket_service_new();
	g_signal_connect(service, "incoming", G_CALLBACK(on_incoming), agent);

	/*
	 * An active service starts accepting inside add_address(), so that
	 * is the call the context has to be pushed around.
	 */
	g_main_context_push_thread_default(self->context);
	bound = g_socket_listener_add_address(G_SOCKET_LISTENER(service),
	                                      address, G_SOCKET_TYPE_STREAM,
	                                      G_SOCKET_PROTOCOL_DEFAULT, NULL,
	                                      NULL, error);
	g_main_context_pop_thread_default(self->context);

	if (!bound) {
		g_prefix_error(error, "binding the egress gate for %s: ", agent_id);
		return NULL;
	}

	if (g_chmod(agent->path, 0600) != 0) {
		g_socket_listener_close(G_SOCKET_LISTENER(service));
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
		            "could not restrict %s: %s", agent->path,
		            g_strerror(errno));
		return NULL;
	}

	agent->service = g_steal_pointer(&service);

	return g_strdup(agent->path);
}

void
clawt_egress_gate_close(
	ClawtEgressGate *self,
	const gchar     *agent_id
){
	AgentGate *agent;

	g_return_if_fail(CLAWT_IS_EGRESS_GATE(self));

	agent = (agent_id != NULL) ? g_hash_table_lookup(self->agents, agent_id)
	                           : NULL;

	if (agent != NULL)
		agent_gate_stop_service(agent);
}

void
clawt_egress_gate_stop(
	ClawtEgressGate *self
){
	GHashTableIter iter;
	gpointer value;

	g_return_if_fail(CLAWT_IS_EGRESS_GATE(self));

	/*
	 * A fresh one afterwards, so a gate stopped by one daemon stop can
	 * be opened again by the next start in the same process.
	 */
	if (self->cancellable != NULL) {
		g_cancellable_cancel(self->cancellable);
		g_clear_object(&self->cancellable);
		self->cancellable = g_cancellable_new();
	}

	if (self->agents == NULL)
		return;

	g_hash_table_iter_init(&iter, self->agents);

	while (g_hash_table_iter_next(&iter, NULL, &value))
		agent_gate_stop_service(value);
}

/* ── What the clients say ────────────────────────────────────────── */

const gchar *
clawt_egress_network_label(void)
{
	return "Network";
}

const gchar *
clawt_egress_network_choice_label(
	const gchar *nick
){
	if (g_strcmp0(nick, "open") == 0)
		return "Open";

	if (g_strcmp0(nick, "gated") == 0)
		return "Through clawtilla's gate";

	if (g_strcmp0(nick, "none") == 0)
		return "None";

	return nick;
}

const gchar *
clawt_egress_network_choice_explanation(
	const gchar *nick
){
	if (g_strcmp0(nick, "open") == 0)
		return "Reaches everything this machine can, your LAN and tailnet "
		       "included.";

	if (g_strcmp0(nick, "gated") == 0)
		return "Public internet only, through a proxy that refuses this "
		       "machine, your LAN, tailnet and cloud metadata.";

	if (g_strcmp0(nick, "none") == 0)
		return "No network; only provider keys clawtilla brokers still "
		       "reach their provider.";

	return NULL;
}

const gchar *
clawt_egress_network_notice(
	const gchar *network,
	const gchar *confine
){
	gboolean confined = g_strcmp0(confine, "bwrap") == 0;

	if (confined)
		return NULL;

	if (network != NULL && *network != '\0' &&
	    g_strcmp0(network, "open") != 0)
		return "Gating the network needs the sandbox: without it the "
		       "agent could ignore the gate. This agent is disabled "
		       "(shadow) until sandboxing is on or the network is open.";

	return "Turn on sandboxing first. Outside the sandbox the agent's "
	       "CLI can ignore a proxy, so only Open can be given.";
}

gchar *
clawt_egress_status_line(
	const gchar *network,
	guint        refused_today
){
	if (g_strcmp0(network, "gated") == 0)
		return g_strdup_printf("egress: gated (%u refused today)",
		                       refused_today);

	if (g_strcmp0(network, "none") == 0)
		return g_strdup("egress: none (brokered provider keys only)");

	return g_strdup("egress: open");
}

gchar *
clawt_egress_refusal_line(
	const gchar *host,
	guint        port,
	const gchar *address,
	const gchar *reason
){
	GString *out = g_string_new(NULL);

	g_string_append_printf(out, "%s:%u", host != NULL ? host : "?", port);

	if (address != NULL && *address != '\0' &&
	    g_strcmp0(address, host) != 0)
		g_string_append_printf(out, " (%s)", address);

	if (reason != NULL && *reason != '\0')
		g_string_append_printf(out, " -- %s", reason);

	return g_string_free(out, FALSE);
}

const gchar *
clawt_egress_allowlist_label(void)
{
	return "Private addresses gated agents may reach";
}

const gchar *
clawt_egress_allowlist_explanation(void)
{
	return "A gated agent reaches the public internet only. Add an "
	       "address (100.101.102.103), a network (192.168.1.0/24) or a "
	       "host name, with an optional :port, to let it reach that "
	       "too -- a tailnet service, say. Cloud metadata can never be "
	       "opened.";
}
