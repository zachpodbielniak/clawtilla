/*
 * test-http-redirect.c - A credential is never carried across a redirect
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * libsoup follows a 3xx by default and keeps every request header,
 * Authorization included, and on a 307 or 308 it sends the body again.
 * So a notifier token, an OAuth client secret or a Matrix password went
 * to whatever host the first server named.  clawt_http_message_new() is
 * where the flag is set and clawt_http_redirect_refusal() is where a
 * refused redirect becomes an error; both are checked here as data.
 *
 * Hermetic on purpose: `make test` may open no network socket, so the
 * server that answers 302 listens on a unix socket and the session
 * reaches it through `remote-connectable` -- the URL's host is never
 * resolved.  That is the live half: a credentialed request against a
 * real 3xx, which must stop there with the credential sent once, beside
 * a control that does follow.  tests/test-http-constructors.sh checks the
 * third part: that no SoupMessage under src/ is made anywhere but
 * clawt-http.c.
 */

#include <clawtilla.h>
#include "clawt-http.h"

#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>
#include <string.h>

#include "clawt-test-util.h"

/* A credentialed request refuses redirects; a public one follows them. */
static void
test_flags_follow_the_declaration(void)
{
	g_autoptr(SoupMessage) secret = NULL;
	g_autoptr(SoupMessage) open = NULL;

	secret = clawt_http_message_new("POST", "https://ntfy.example/topic",
	                                CLAWT_HTTP_CARRIES_CREDENTIALS);
	open = clawt_http_message_new("GET", "https://images.example/f.qcow2",
	                              CLAWT_HTTP_NO_CREDENTIALS);

	g_assert_nonnull(secret);
	g_assert_nonnull(open);
	g_assert_true((soup_message_get_flags(secret) &
	               SOUP_MESSAGE_NO_REDIRECT) != 0);
	g_assert_true((soup_message_get_flags(open) &
	               SOUP_MESSAGE_NO_REDIRECT) == 0);
	g_assert_cmpstr(soup_message_get_method(secret), ==, "POST");
}

/* Not a URL is NULL, as soup_message_new() says, not a crash later. */
static void
test_unparsable_url_is_null(void)
{
	g_autoptr(SoupMessage) message = NULL;

	message = clawt_http_message_new("GET", "not a url",
	                                 CLAWT_HTTP_CARRIES_CREDENTIALS);
	g_assert_null(message);
}

/*
 * Every status libsoup would follow is refused, and the refusal names the
 * origin it pointed at -- and neither path, because an ntfy topic and a
 * redirect target's query are both places a secret lives.
 */
static void
test_each_followed_status_is_refused(void)
{
	static const guint statuses[] = { 301, 302, 303, 307, 308 };
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(statuses); i++) {
		g_autoptr(GError) error = NULL;

		error = clawt_http_redirect_refusal(
			"https://ntfy.example/secret-topic", statuses[i],
			"https://collector.example:8443/grab?token=abc");

		g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED);
		g_assert_nonnull(strstr(error->message,
		                        "https://collector.example:8443"));
		g_assert_nonnull(strstr(error->message, "https://ntfy.example"));
		g_assert_null(strstr(error->message, "secret-topic"));
		g_assert_null(strstr(error->message, "token=abc"));
		g_assert_null(strstr(error->message, "/grab"));
	}
}

/* Anything else is the caller's to judge, 304 and 300 included. */
static void
test_other_statuses_pass(void)
{
	static const guint statuses[] = { 0, 200, 202, 204, 300, 304, 400,
	                                  401, 404, 500 };
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(statuses); i++) {
		g_autoptr(GError) error = NULL;

		error = clawt_http_redirect_refusal("https://a.example/x",
		                                    statuses[i],
		                                    "https://b.example/");
		g_assert_no_error(error);
	}
}

/*
 * A relative Location resolves to the same server, which still refuses
 * -- the person fixes it the same way -- and the path is named, since it
 * is the one they should configure on a host they already chose.
 */
static void
test_same_origin_names_the_path(void)
{
	g_autoptr(GError) error = NULL;

	error = clawt_http_redirect_refusal("https://gotify.example/message",
	                                    307, "/message/");

	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED);
	g_assert_nonnull(strstr(error->message, "same server"));
	g_assert_nonnull(strstr(error->message, "/message/"));
}

/* Plain http to https on one host is a different origin, said so. */
static void
test_scheme_change_is_another_origin(void)
{
	g_autoptr(GError) error = NULL;

	error = clawt_http_redirect_refusal("http://matrix.example/_matrix/x",
	                                    301,
	                                    "https://matrix.example/_matrix/x");

	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED);
	g_assert_nonnull(strstr(error->message, "https://matrix.example"));
	g_assert_null(strstr(error->message, "same server"));
}

/* An IPv6 literal keeps its brackets, or the port reads as an address. */
static void
test_ipv6_origin_is_bracketed(void)
{
	g_autoptr(GError) error = NULL;

	error = clawt_http_redirect_refusal("https://a.example/x", 302,
	                                    "http://[fd00::1]:8080/y");

	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED);
	g_assert_nonnull(strstr(error->message, "http://[fd00::1]:8080"));
}

/* A redirect with nowhere to go is still a redirect, and still refused. */
static void
test_missing_location_is_refused(void)
{
	g_autoptr(GError) error = NULL;

	error = clawt_http_redirect_refusal("https://a.example/x", 302, NULL);

	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED);
	g_assert_nonnull(strstr(error->message, "no Location"));
}

/* Before any reply there is nothing to refuse, for either kind. */
static void
test_check_passes_without_a_reply(void)
{
	g_autoptr(SoupMessage) secret = NULL;
	g_autoptr(SoupMessage) open = NULL;
	g_autoptr(GError) error = NULL;

	secret = clawt_http_message_new("GET", "https://a.example/",
	                                CLAWT_HTTP_CARRIES_CREDENTIALS);
	open = clawt_http_message_new("GET", "https://a.example/",
	                              CLAWT_HTTP_NO_CREDENTIALS);

	g_assert_true(clawt_http_check_redirect(secret, &error));
	g_assert_no_error(error);
	g_assert_true(clawt_http_check_redirect(open, &error));
	g_assert_no_error(error);
}

/* ── A live 3xx, on a unix socket ────────────────────────────────── */

typedef struct {
	guint  requests;
	guint  with_credential;
	gchar *last_path;
} RedirectServer;

/* /topic answers 302 to another host; /grab is where it points. */
static void
redirect_handler(
	SoupServer        *server,
	SoupServerMessage *message,
	const char        *path,
	GHashTable        *query,
	gpointer           user_data
){
	RedirectServer *seen = user_data;
	SoupMessageHeaders *in = soup_server_message_get_request_headers(message);

	seen->requests++;
	g_free(seen->last_path);
	seen->last_path = g_strdup(path);

	if (soup_message_headers_get_one(in, "Authorization") != NULL)
		seen->with_credential++;

	if (g_strcmp0(path, "/topic") == 0) {
		soup_server_message_set_redirect(message, 302,
		                                 "http://collector.test/grab");
		return;
	}

	soup_server_message_set_status(message, 200, NULL);
	soup_server_message_set_response(message, "text/plain",
	                                 SOUP_MEMORY_STATIC, "grabbed", 7);
}

static gboolean
redirect_watchdog(
	gpointer user_data
){
	g_error("the redirect server did not answer within 10 seconds");
	return G_SOURCE_REMOVE;
}

static void
on_redirect_sent(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	GBytes **out = user_data;

	*out = soup_session_send_and_read_finish(SOUP_SESSION(source), result,
	                                         NULL);
	if (*out == NULL)
		*out = g_bytes_new(NULL, 0);
}

static void
send_and_wait(
	SoupSession *session,
	SoupMessage *message
){
	g_autoptr(GBytes) body = NULL;
	guint watchdog;

	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT,
	                                 NULL, on_redirect_sent, &body);

	watchdog = g_timeout_add_seconds(10, redirect_watchdog, NULL);
	while (body == NULL)
		g_main_context_iteration(NULL, TRUE);
	g_source_remove(watchdog);
}

/*
 * The whole claim, against a server: a message built carrying a
 * credential stops at the 302, the credential was sent exactly once (to
 * the host it was meant for), and clawt_http_check_redirect() turns the
 * reply into a refusal naming where it pointed.  The control is the same
 * request built without one, which libsoup follows -- so the server is
 * known to be redirecting, and the first arm is not passing because
 * nothing redirected.
 */
static void
test_live_redirect_is_not_followed(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("clawt-http-XXXXXX", NULL);
	g_autofree gchar *path = g_build_filename(dir, "server.sock", NULL);
	g_autoptr(SoupServer) server = soup_server_new(NULL, NULL);
	g_autoptr(GSocketAddress) address = g_unix_socket_address_new(path);
	g_autoptr(SoupSession) session = NULL;
	g_autoptr(SoupMessage) secret = NULL;
	g_autoptr(SoupMessage) open = NULL;
	g_autoptr(GError) error = NULL;
	RedirectServer seen = { 0 };

	soup_server_add_handler(server, NULL, redirect_handler, &seen, NULL);
	g_assert_true(soup_server_listen(server, address, 0, &error));
	g_assert_no_error(error);
	session = soup_session_new_with_options("remote-connectable", address,
	                                        NULL);

	secret = clawt_http_message_new("POST", "http://ntfy.test/topic",
	                                CLAWT_HTTP_CARRIES_CREDENTIALS);
	soup_message_headers_append(soup_message_get_request_headers(secret),
	                            "Authorization", "Bearer tk-secret");
	send_and_wait(session, secret);

	g_assert_cmpuint(soup_message_get_status(secret), ==, 302);
	g_assert_cmpuint(seen.requests, ==, 1);
	g_assert_cmpuint(seen.with_credential, ==, 1);
	g_assert_cmpstr(seen.last_path, ==, "/topic");
	g_assert_false(clawt_http_check_redirect(secret, &error));
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED);
	g_assert_nonnull(strstr(error->message, "http://collector.test"));
	g_assert_null(strstr(error->message, "tk-secret"));
	g_clear_error(&error);

	/* The control: the same request without a credential follows. */
	open = clawt_http_message_new("GET", "http://ntfy.test/topic",
	                              CLAWT_HTTP_NO_CREDENTIALS);
	send_and_wait(session, open);

	g_assert_cmpuint(soup_message_get_status(open), ==, 200);
	g_assert_cmpuint(seen.requests, ==, 3);
	g_assert_cmpstr(seen.last_path, ==, "/grab");
	g_assert_cmpuint(seen.with_credential, ==, 1);
	g_assert_true(clawt_http_check_redirect(open, NULL));

	soup_server_disconnect(server);
	while (g_main_context_iteration(NULL, FALSE))
		;
	g_free(seen.last_path);
	clawt_test_remove_tree(dir);
}

int
main(
	int   argc,
	char *argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/http-redirect/flags",
	                test_flags_follow_the_declaration);
	g_test_add_func("/http-redirect/unparsable", test_unparsable_url_is_null);
	g_test_add_func("/http-redirect/followed-statuses",
	                test_each_followed_status_is_refused);
	g_test_add_func("/http-redirect/other-statuses", test_other_statuses_pass);
	g_test_add_func("/http-redirect/same-origin",
	                test_same_origin_names_the_path);
	g_test_add_func("/http-redirect/scheme-change",
	                test_scheme_change_is_another_origin);
	g_test_add_func("/http-redirect/ipv6", test_ipv6_origin_is_bracketed);
	g_test_add_func("/http-redirect/no-location",
	                test_missing_location_is_refused);
	g_test_add_func("/http-redirect/live-3xx-not-followed",
	                test_live_redirect_is_not_followed);
	g_test_add_func("/http-redirect/no-reply",
	                test_check_passes_without_a_reply);

	return g_test_run();
}
