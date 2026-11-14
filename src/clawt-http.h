/*
 * clawt-http.h - Outbound HTTP requests, and where they may be redirected
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Private to the library: not in <clawtilla.h>, not scanned into the GIR.
 * Every SoupMessage clawtilla builds is made here, so the decision about
 * redirects is taken once rather than at each of a dozen call sites --
 * tests/test-http-constructors.sh fails the suite when a raw
 * soup_message_new() appears anywhere else under src/.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>
#include <gio/gio.h>
#include <libsoup/soup.h>

G_BEGIN_DECLS

/**
 * ClawtHttpCredentials:
 * @CLAWT_HTTP_CARRIES_CREDENTIALS: the request carries a credential -- an
 *   `Authorization` header, an API key header, a password, a client secret,
 *   a refresh or device code in its body -- so it is never redirected
 * @CLAWT_HTTP_NO_CREDENTIALS: the request carries nothing secret, so a
 *   redirect is followed as libsoup would by default
 *
 * What a request carries, which decides whether it may follow a redirect.
 *
 * libsoup follows a 3xx by default and keeps every request header it was
 * given, `Authorization` and `X-Gotify-Key` included; on a 307 or 308 it
 * sends the body again too.  So a bearer token, an OAuth client secret or
 * a Matrix password went to whatever host the first server named.  There
 * is deliberately no default: the caller says which it is.
 */
typedef enum {
	CLAWT_HTTP_CARRIES_CREDENTIALS = 0,
	CLAWT_HTTP_NO_CREDENTIALS
} ClawtHttpCredentials;

/**
 * clawt_http_message_new:
 * @method: the HTTP method
 * @url: the URL to send it to
 * @credentials: whether the request will carry a credential
 *
 * Creates a #SoupMessage, as soup_message_new() does, with redirects
 * refused when @credentials is %CLAWT_HTTP_CARRIES_CREDENTIALS.
 *
 * A refused redirect comes back from libsoup as an ordinary 3xx reply.
 * Finish the request with clawt_http_send_and_read_finish() or
 * clawt_http_send_and_read(), or call clawt_http_check_redirect(), to
 * turn that into an error naming where the server tried to send it.
 *
 * Returns: (transfer full) (nullable): the message, or %NULL when @url
 *   is not one libsoup can parse
 */
SoupMessage *
clawt_http_message_new(
	const gchar          *method,
	const gchar          *url,
	ClawtHttpCredentials  credentials
);

/**
 * clawt_http_redirect_refusal:
 * @requested_url: the URL the request was sent to
 * @status: the status the server answered with
 * @location: (nullable): the reply's `Location` header
 *
 * Decides whether a reply is a redirect a credentialed request must not
 * follow, and if it is, says so in words that name where it pointed.
 *
 * Pure: no message and no network, so the decision is testable as data.
 * Only the statuses libsoup would follow count (301, 302, 303, 307 and
 * 308); a 304 or a 300 is the caller's to judge.  A relative @location
 * is resolved against @requested_url.  Only the origin of the target is
 * named -- its path and query can carry a token of their own, and this
 * message ends up in logs and in front of an agent.
 *
 * Returns: (transfer full) (nullable): a %CLAWT_ERROR_PERMISSION_DENIED
 *   error when the reply is a redirect, or %NULL when it is not
 */
GError *
clawt_http_redirect_refusal(
	const gchar *requested_url,
	guint        status,
	const gchar *location
);

/**
 * clawt_http_check_redirect:
 * @message: a message whose reply has arrived
 * @error: (out) (optional): return location for a #GError
 *
 * Fails when @message refused a redirect and the server sent one.
 *
 * A message created with %CLAWT_HTTP_NO_CREDENTIALS has already followed
 * its redirects by the time this is called, so it always passes.
 *
 * Returns: %TRUE when the reply is not a refused redirect
 */
gboolean
clawt_http_check_redirect(
	SoupMessage  *message,
	GError      **error
);

/**
 * clawt_http_send_and_read_finish:
 * @session: the session the request was sent on
 * @result: the result passed to the callback
 * @error: (out) (optional): return location for a #GError
 *
 * soup_session_send_and_read_finish(), then clawt_http_check_redirect()
 * on the message the result belongs to.
 *
 * A refused redirect is an error here rather than a body, because every
 * caller reads a body as the answer: a token endpoint's 302 parsed into
 * "no access_token in the response", which sends the reader to the
 * provider instead of to the URL that is wrong.
 *
 * Returns: (transfer full) (nullable): the body, or %NULL on error
 */
GBytes *
clawt_http_send_and_read_finish(
	SoupSession   *session,
	GAsyncResult  *result,
	GError       **error
);

/**
 * clawt_http_send_and_read:
 * @session: the session to send on
 * @message: the message to send
 * @cancellable: (nullable): a #GCancellable
 * @error: (out) (optional): return location for a #GError
 *
 * The synchronous form of clawt_http_send_and_read_finish().
 *
 * Returns: (transfer full) (nullable): the body, or %NULL on error
 */
GBytes *
clawt_http_send_and_read(
	SoupSession   *session,
	SoupMessage   *message,
	GCancellable  *cancellable,
	GError       **error
);

G_END_DECLS
