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
