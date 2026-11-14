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
 * Hermetic on purpose: `make test` may open no network socket, so a
 * loopback server that answers 302 is not available.  What a socket
 * would add is libsoup honouring SOUP_MESSAGE_NO_REDIRECT, which is
 * libsoup's own contract; what is clawtilla's -- that the flag is set,
 * and that the answer it produces names the right thing -- is here.
 * tests/test-http-constructors.sh checks the third part: that no
 * SoupMessage under src/ is made anywhere but clawt-http.c.
 */

#include <clawtilla.h>
#include "clawt-http.h"

#include <string.h>

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
	g_test_add_func("/http-redirect/no-reply",
	                test_check_passes_without_a_reply);

	return g_test_run();
}
