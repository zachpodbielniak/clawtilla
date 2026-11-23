/*
 * clawt-http.c - Outbound HTTP requests, and where they may be redirected
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "clawt-http.h"

/*
 * The origin of a parsed URI, scheme://host[:port], and nothing after it.
 *
 * The path is left out on purpose: an ntfy topic is a capability, a
 * Gotify URL can be one, and a provider's redirect target may carry a
 * code in its query.  This string goes into a GError that reaches the
 * daemon log and, through a tool reply, an agent's transcript.
 */
static gchar *
uri_origin(
	GUri *uri
){
	const gchar *scheme;
	const gchar *host;

	scheme = g_uri_get_scheme(uri);
	host = g_uri_get_host(uri);

	/* g_uri_join() brackets an IPv6 literal, which a printf would not. */
	return g_uri_join(G_URI_FLAGS_NONE,
	                  scheme != NULL ? scheme : "http",
	                  NULL,
	                  host != NULL ? host : "",
	                  g_uri_get_port(uri),
	                  "",
	                  NULL,
	                  NULL);
}

SoupMessage *
clawt_http_message_new(
	const gchar          *method,
	const gchar          *url,
	ClawtHttpCredentials  credentials
){
	g_autoptr(SoupMessage) message = NULL;

	g_return_val_if_fail(method != NULL, NULL);
	g_return_val_if_fail(url != NULL, NULL);

	message = soup_message_new(method, url);

	if (message == NULL)
		return NULL;

	/*
	 * Set before anything else can touch the message, so no caller can
	 * add the credential first and the flag later -- or forget it.
	 */
	if (credentials == CLAWT_HTTP_CARRIES_CREDENTIALS)
		soup_message_add_flags(message, SOUP_MESSAGE_NO_REDIRECT);

	return (SoupMessage *)g_steal_pointer(&message);
}

GError *
clawt_http_redirect_refusal(
	const gchar *requested_url,
	guint        status,
	const gchar *location
){
	g_autoptr(GUri) requested = NULL;
	g_autoptr(GUri) target = NULL;
	g_autofree gchar *from = NULL;
	g_autofree gchar *to = NULL;

	/*
	 * Exactly the set libsoup's own redirect handler follows.  A 304 is
	 * a cache answer, and a 300 is never followed automatically, so
	 * neither is a redirect anybody's credential could ride.
	 */
	switch (status) {
	case SOUP_STATUS_MOVED_PERMANENTLY:
	case SOUP_STATUS_FOUND:
	case SOUP_STATUS_SEE_OTHER:
	case SOUP_STATUS_TEMPORARY_REDIRECT:
	case SOUP_STATUS_PERMANENT_REDIRECT:
		break;
	default:
		return NULL;
	}

	requested = requested_url != NULL
		? g_uri_parse(requested_url, G_URI_FLAGS_NONE, NULL)
		: NULL;
	from = requested != NULL ? uri_origin(requested) : g_strdup("the server");

	/* A relative Location is legal (RFC 9110) and common. */
	if (location != NULL && *location != '\0') {
		if (requested != NULL)
			target = g_uri_parse_relative(requested, location,
			                              G_URI_FLAGS_NONE, NULL);
		else
			target = g_uri_parse(location, G_URI_FLAGS_NONE, NULL);
	}

	if (target == NULL) {
		return g_error_new(CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED,
		                   "%s answered %u, a redirect to %s; clawtilla "
		                   "never sends a credential across a redirect, so "
		                   "the request was not repeated -- check the URL "
		                   "configured for it",
		                   from, status,
		                   location != NULL && *location != '\0'
		                   ? "an address that does not parse"
		                   : "nowhere (no Location header)");
	}

	to = uri_origin(target);

	/*
	 * Same origin still refuses: a trailing slash or a moved endpoint is
	 * the usual cause, and the fix -- the URL it names -- is the same.
	 * The path is safe to name here because it is the path the person
	 * should configure, on a host they already configured.
	 */
	if (g_strcmp0(from, to) == 0) {
		return g_error_new(CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED,
		                   "%s answered %u, a redirect to %s on the same "
		                   "server; clawtilla never sends a credential "
		                   "across a redirect, so the request was not "
		                   "repeated -- configure that path instead",
		                   from, status, g_uri_get_path(target));
	}

	return g_error_new(CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED,
	                   "%s answered %u, a redirect to %s; clawtilla never "
	                   "sends a credential across a redirect, so the "
	                   "request was not repeated -- if %s is where it "
	                   "should go, configure that address instead",
	                   from, status, to, to);
}

gboolean
clawt_http_check_redirect(
	SoupMessage  *message,
	GError      **error
){
	g_autofree gchar *requested_url = NULL;
	GError *refusal;

	g_return_val_if_fail(SOUP_IS_MESSAGE(message), FALSE);

	/*
	 * A message that may follow redirects already has; one that reached
	 * here with a 3xx anyway (more than libsoup's limit of them) is not
	 * this function's business.
	 */
	if ((soup_message_get_flags(message) & SOUP_MESSAGE_NO_REDIRECT) == 0)
		return TRUE;

	requested_url = g_uri_to_string(soup_message_get_uri(message));
	refusal = clawt_http_redirect_refusal(
		requested_url, soup_message_get_status(message),
		soup_message_headers_get_one(
			soup_message_get_response_headers(message), "Location"));

	if (refusal == NULL)
		return TRUE;

	g_propagate_error(error, refusal);
	return FALSE;
}

GBytes *
clawt_http_send_and_read_finish(
	SoupSession   *session,
	GAsyncResult  *result,
	GError       **error
){
	g_autoptr(GBytes) body = NULL;
	SoupMessage *message;

	g_return_val_if_fail(SOUP_IS_SESSION(session), NULL);

	/* The message the result belongs to, which callbacks never get handed. */
	message = soup_session_get_async_result_message(session, result);
	body = soup_session_send_and_read_finish(session, result, error);

	if (body == NULL)
		return NULL;

	if (message != NULL && !clawt_http_check_redirect(message, error))
		return NULL;

	return (GBytes *)g_steal_pointer(&body);
}

GBytes *
clawt_http_send_and_read(
	SoupSession   *session,
	SoupMessage   *message,
	GCancellable  *cancellable,
	GError       **error
){
	g_autoptr(GBytes) body = NULL;

	g_return_val_if_fail(SOUP_IS_SESSION(session), NULL);
	g_return_val_if_fail(SOUP_IS_MESSAGE(message), NULL);

	body = soup_session_send_and_read(session, message, cancellable, error);

	if (body == NULL)
		return NULL;

	if (!clawt_http_check_redirect(message, error))
		return NULL;

	return (GBytes *)g_steal_pointer(&body);
}

/* ── Bounded request bodies, for any SoupServer here ─────────────── */

/*
 * RefusalClose:
 *
 * A rejected HTTP connection may already have more upload bytes queued
 * in TCP. Closing it with unread bytes sends RST and can hide the 413
 * from ordinary clients. Drain into a fixed buffer after replying, but
 * only for one second; an idle or endless sender cannot retain the
 * connection indefinitely. The same deadline also bounds the write.
 */
typedef struct {
	GIOStream *stream;
	GCancellable *cancellable;
	GSource *deadline;
	guchar buffer[8192];
} RefusalClose;

/**
 * on_refusal_closed:
 * @source: stolen connection
 * @result: completion of closing the connection
 * @user_data: owned rejection state
 *
 * Release every resource once the asynchronous close has finished.
 */
static void
on_refusal_closed(GObject *source, GAsyncResult *result, gpointer user_data)
{
	RefusalClose *state = user_data;

	g_io_stream_close_finish(G_IO_STREAM(source), result, NULL);
	g_object_unref(state->stream);
	g_object_unref(state->cancellable);
	g_free(state);
}

/**
 * close_refusal:
 * @state: owned rejection state
 *
 * Stop the deadline before closing. Do not pass its cancelled token to
 * close: the descriptor must actually close even after a timeout.
 */
static void
close_refusal(RefusalClose *state)
{
	g_source_destroy(state->deadline);
	g_source_unref(state->deadline);
	g_io_stream_close_async(state->stream, G_PRIORITY_DEFAULT, NULL,
		on_refusal_closed, state);
}

/**
 * on_refusal_drained:
 * @source: rejected connection's input stream
 * @result: completion of discarding one bounded buffer
 * @user_data: owned rejection state
 *
 * Discard only while the grace period remains. The peer normally closes
 * after reading 413; otherwise cancellation ends the read at the deadline.
 */
static void
on_refusal_drained(GObject *source, GAsyncResult *result, gpointer user_data)
{
	RefusalClose *state = user_data;
	gssize count;

	count = g_input_stream_read_finish(G_INPUT_STREAM(source), result, NULL);
	if (count <= 0 || g_cancellable_is_cancelled(state->cancellable)) {
		close_refusal(state);
		return;
	}
	g_input_stream_read_async(G_INPUT_STREAM(source), state->buffer,
		sizeof(state->buffer), G_PRIORITY_DEFAULT, state->cancellable,
		on_refusal_drained, state);
}

/**
 * on_refusal_written:
 * @source: output stream of the rejected connection
 * @result: completion of writing the fixed HTTP response
 * @user_data: owned rejection state
 *
 * Let in-flight upload bytes drain after writing the response so that
 * closing the socket does not replace a usable HTTP error with TCP RST.
 */
static void
on_refusal_written(GObject *source, GAsyncResult *result, gpointer user_data)
{
	RefusalClose *state = user_data;

	if (!g_output_stream_write_all_finish(G_OUTPUT_STREAM(source), result,
		NULL, NULL)) {
		close_refusal(state);
		return;
	}
	g_input_stream_read_async(g_io_stream_get_input_stream(state->stream),
		state->buffer, sizeof(state->buffer), G_PRIORITY_DEFAULT,
		state->cancellable, on_refusal_drained, state);
}

/**
 * on_refusal_deadline:
 * @user_data: rejection state with an outstanding write or read
 *
 * Bound cleanup even if the sender neither finishes uploading nor closes.
 *
 * Returns: %G_SOURCE_REMOVE after cancelling the outstanding operation
 */
static gboolean
on_refusal_deadline(gpointer user_data)
{
	RefusalClose *state = user_data;

	g_cancellable_cancel(state->cancellable);
	return G_SOURCE_REMOVE;
}

/**
 * refuse_oversized_body:
 * @message: request that exceeded the configured cap
 *
 * Setting a status while libsoup is reading a body does not interrupt
 * that read. Steal the HTTP/1 connection to stop accumulation immediately,
 * then reply and close with a bounded drain period. Stealing can release
 * the last reference to @message, so do not access it afterward.
 */
static void
refuse_oversized_body(SoupServerMessage *message)
{
	static const gchar response[] =
		"HTTP/1.1 413 Content Too Large\r\n"
		"Connection: close\r\n"
		"Content-Length: 0\r\n\r\n";
	GIOStream *stream;
	RefusalClose *state;

	stream = soup_server_message_steal_connection(message);
	if (stream == NULL)
		return;
	state = g_new0(RefusalClose, 1);
	state->stream = stream;
	state->cancellable = g_cancellable_new();
	state->deadline = g_timeout_source_new(1000);
	g_source_set_callback(state->deadline, on_refusal_deadline, state, NULL);
	g_source_attach(state->deadline, g_main_context_get_thread_default());
	g_output_stream_write_all_async(g_io_stream_get_output_stream(stream),
		response, sizeof(response) - 1, G_PRIORITY_DEFAULT, state->cancellable,
		on_refusal_written, state);
}

/*
 * on_got_chunk:
 * @message: request owning the bounded body buffer
 * @chunk: newly received decoded body bytes
 * @user_data: ingress defining the byte limit
 *
 * libsoup's automatic accumulation is disabled before any body arrives.
 * Only retain a chunk if the total fits, using subtraction to avoid
 * overflowing the byte count on hostile input.
 */
static void
on_got_chunk(SoupServerMessage *message, GBytes *chunk, gpointer user_data)
{
	gsize max_body_bytes = GPOINTER_TO_SIZE(user_data);
	SoupMessageBody *body = g_object_get_data(G_OBJECT(message),
		"clawt-bounded-body");
	gsize length = g_bytes_get_size(chunk);

	if (length > max_body_bytes - (gsize)body->length) {
		refuse_oversized_body(message);
		return;
	}
	soup_message_body_append_bytes(body, chunk);
}

/*
 * on_got_headers:
 * @message: request whose headers have just arrived
 * @user_data: ingress defining the byte limit
 *
 * Refuse a declared oversized body before reading it. Other requests
 * use a separately owned, bounded buffer populated by on_got_chunk().
 */
static void
on_got_headers(SoupServerMessage *message, gpointer user_data)
{
	gsize max_body_bytes = GPOINTER_TO_SIZE(user_data);
	SoupMessageHeaders *headers =
		soup_server_message_get_request_headers(message);
	goffset declared = soup_message_headers_get_content_length(headers);

	soup_message_body_set_accumulate(
		soup_server_message_get_request_body(message), FALSE);
	if (declared > 0 && (guint64)declared > max_body_bytes) {
		refuse_oversized_body(message);
		return;
	}
	g_object_set_data_full(G_OBJECT(message), "clawt-bounded-body",
		soup_message_body_new(), (GDestroyNotify)soup_message_body_unref);
	g_signal_connect(message, "got-chunk", G_CALLBACK(on_got_chunk), user_data);
}

static void
on_request_started(SoupServer        *server,
                   SoupServerMessage *message,
                   gpointer           user_data)
{
    (void)server;

    g_signal_connect(message, "got-headers", G_CALLBACK(on_got_headers),
                     user_data);
}


void
clawt_http_server_limit_bodies(
	SoupServer *server,
	gsize       max_body_bytes
){
	g_return_if_fail(SOUP_IS_SERVER(server));

	g_signal_connect(server, "request-started", G_CALLBACK(on_request_started),
		GSIZE_TO_POINTER(max_body_bytes));
}

SoupMessageBody *
clawt_http_server_message_body(
	SoupServerMessage *message
){
	return g_object_get_data(G_OBJECT(message), "clawt-bounded-body");
}
