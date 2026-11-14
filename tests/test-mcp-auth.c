/*
 * test-mcp-auth.c - Finding out how to sign in to a remote MCP server
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Every document discovery reads, every decision it takes and every
 * request it builds is tested here as data.  None of it opens a socket:
 * the three network legs are thin wrappers around these functions, and
 * the ways discovery goes wrong -- a document about some other server, a
 * mix-up between issuers, an endpoint in the clear, a hostile hint aimed
 * at loopback -- are all decisions made on text.
 */

#include "clawtilla.h"
#include "clawt-http.h"
#include "clawt-test-util.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <string.h>
#include <sys/stat.h>

/* ── The 401's challenge ─────────────────────────────────────────── */

static void
test_challenge_is_read_quoted_or_bare(
	void
){
	g_autofree gchar *quoted = NULL;
	g_autofree gchar *bare = NULL;
	g_autofree gchar *later = NULL;
	g_autofree gchar *none = NULL;
	g_autofree gchar *absent = NULL;

	quoted = clawt_mcp_auth_parse_challenge(
		"Bearer resource_metadata=\"https://mcp.example/.well-known/"
		"oauth-protected-resource\"");
	bare = clawt_mcp_auth_parse_challenge(
		"Bearer realm=\"mcp\", resource_metadata=https://mcp.example/prm");
	later = clawt_mcp_auth_parse_challenge(
		"Basic realm=\"x\", Bearer error=\"invalid_token\", "
		"resource_metadata=\"https://mcp.example/prm\"");
	none = clawt_mcp_auth_parse_challenge("Bearer realm=\"mcp\"");
	absent = clawt_mcp_auth_parse_challenge(NULL);

	g_assert_cmpstr(quoted, ==,
	                "https://mcp.example/.well-known/oauth-protected-resource");
	g_assert_cmpstr(bare, ==, "https://mcp.example/prm");
	g_assert_cmpstr(later, ==, "https://mcp.example/prm");
	g_assert_null(none);
	g_assert_null(absent);
}

/*
 * A realm is the server's free text.  Searching the whole header for the
 * parameter name would read a realm *about* resource_metadata as the
 * parameter itself and fetch whatever followed it.
 */
static void
test_challenge_ignores_the_name_inside_a_value(
	void
){
	g_autofree gchar *found = NULL;

	found = clawt_mcp_auth_parse_challenge(
		"Bearer realm=\"resource_metadata=http://127.0.0.1/evil\"");

	g_assert_null(found);
}

/* ── What may be dialled ─────────────────────────────────────────── */

static void
test_https_is_allowed_and_plain_http_is_not(
	void
){
	const gchar *server = "https://mcp.example/mcp";

	g_assert_true(clawt_mcp_auth_url_allowed("https://auth.example/token",
	                                         server));
	g_assert_false(clawt_mcp_auth_url_allowed("http://auth.example/token",
	                                          server));
	g_assert_false(clawt_mcp_auth_url_allowed("ftp://auth.example/", server));
	g_assert_false(clawt_mcp_auth_url_allowed("not a url", server));

	/* Credentials in a URL are refused outright. */
	g_assert_false(clawt_mcp_auth_url_allowed(
		"https://user:pass@auth.example/token", server));
}

/*
 * The hostile-hint case: a public server's 401 naming a loopback URL
 * would aim the daemon at this machine's own unauthenticated services.
 * A loopback server naming loopback is code the operator started.
 */
static void
test_loopback_only_from_loopback(
	void
){
	g_assert_false(clawt_mcp_auth_url_allowed("http://127.0.0.1:9000/prm",
	                                          "https://mcp.example/mcp"));
	g_assert_false(clawt_mcp_auth_url_allowed("https://localhost/prm",
	                                          "https://mcp.example/mcp"));
	g_assert_false(clawt_mcp_auth_url_allowed("http://[::1]:9000/prm",
	                                          "https://mcp.example/mcp"));
	g_assert_false(clawt_mcp_auth_url_allowed("http://127.0.0.2/prm",
	                                          "https://mcp.example/mcp"));

	g_assert_true(clawt_mcp_auth_url_allowed("http://127.0.0.1:9001/token",
	                                         "http://127.0.0.1:9000/mcp"));
	g_assert_true(clawt_mcp_auth_url_allowed("http://localhost:9001/token",
	                                         "http://127.0.0.1:9000/mcp"));

	/* And a loopback server may still name a public https one. */
	g_assert_true(clawt_mcp_auth_url_allowed("https://auth.example/token",
	                                         "http://127.0.0.1:9000/mcp"));
}

/* ── Where to look ───────────────────────────────────────────────── */

static void
test_resource_candidates_follow_rfc_9728(
	void
){
	g_auto(GStrv) with_path = NULL;
	g_auto(GStrv) at_root = NULL;
	g_auto(GStrv) hinted = NULL;

	with_path = clawt_mcp_auth_resource_candidates("https://mcp.example/v1/mcp",
	                                               NULL);
	g_assert_cmpuint(g_strv_length(with_path), ==, 2);
	g_assert_cmpstr(with_path[0], ==,
	                "https://mcp.example/.well-known/oauth-protected-resource"
	                "/v1/mcp");
	g_assert_cmpstr(with_path[1], ==,
	                "https://mcp.example/.well-known/oauth-protected-resource");

	at_root = clawt_mcp_auth_resource_candidates("https://mcp.example/", NULL);
	g_assert_cmpuint(g_strv_length(at_root), ==, 1);

	/* The 401's own hint goes first, and is not repeated. */
	hinted = clawt_mcp_auth_resource_candidates(
		"https://mcp.example:8443/mcp",
		"https://mcp.example:8443/.well-known/oauth-protected-resource");
	g_assert_cmpuint(g_strv_length(hinted), ==, 2);
	g_assert_cmpstr(hinted[0], ==,
	                "https://mcp.example:8443/.well-known/"
	                "oauth-protected-resource");
	g_assert_cmpstr(hinted[1], ==,
	                "https://mcp.example:8443/.well-known/"
	                "oauth-protected-resource/mcp");
}

static void
test_server_candidates_follow_rfc_8414(
	void
){
	g_auto(GStrv) plain = NULL;
	g_auto(GStrv) tenant = NULL;

	plain = clawt_mcp_auth_server_candidates("https://auth.example/");
	g_assert_cmpuint(g_strv_length(plain), ==, 2);
	g_assert_cmpstr(plain[0], ==,
	                "https://auth.example/.well-known/oauth-authorization-server");
	g_assert_cmpstr(plain[1], ==,
	                "https://auth.example/.well-known/openid-configuration");

	tenant = clawt_mcp_auth_server_candidates("https://auth.example/tenant1");
	g_assert_cmpuint(g_strv_length(tenant), ==, 3);
	g_assert_cmpstr(tenant[0], ==,
	                "https://auth.example/.well-known/oauth-authorization-server"
	                "/tenant1");
	g_assert_cmpstr(tenant[2], ==,
	                "https://auth.example/tenant1/.well-known/"
	                "openid-configuration");
}

/* ── The protected-resource document ─────────────────────────────── */

static void
test_resource_document_names_the_server(
	void
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *issuer = NULL;
	g_autofree gchar *scopes = NULL;

	g_assert_true(clawt_mcp_auth_parse_resource(
		"{\"resource\":\"https://mcp.example/mcp\","
		"\"authorization_servers\":[\"https://auth.example\"],"
		"\"scopes_supported\":[\"read\",\"write\"]}",
		-1, "https://mcp.example/mcp", &issuer, &scopes, &error));

	g_assert_no_error(error);
	g_assert_cmpstr(issuer, ==, "https://auth.example");
	g_assert_cmpstr(scopes, ==, "read write");
}

/*
 * RFC 9728 section 3.3.  A document describing a different resource is
 * one a hostile server copied, and following it sends the person to sign
 * in somewhere that is not this server's.
 */
static void
test_resource_document_for_another_server_is_refused(
	void
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *issuer = NULL;

	g_assert_false(clawt_mcp_auth_parse_resource(
		"{\"resource\":\"https://other.example/mcp\","
		"\"authorization_servers\":[\"https://auth.example\"]}",
		-1, "https://mcp.example/mcp", &issuer, NULL, &error));

	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL);
	g_assert_null(issuer);
}

static void
test_resource_document_at_the_origin_is_accepted(
	void
){
	g_autofree gchar *issuer = NULL;
	g_autofree gchar *nothing = NULL;

	/* The origin's own well-known document describes the origin. */
	g_assert_true(clawt_mcp_auth_parse_resource(
		"{\"resource\":\"https://mcp.example/\","
		"\"authorization_servers\":[\"https://auth.example\"]}",
		-1, "https://mcp.example/mcp", &issuer, NULL, NULL));
	g_assert_cmpstr(issuer, ==, "https://auth.example");

	/* No authorization server named: the origin will be assumed. */
	g_assert_true(clawt_mcp_auth_parse_resource(
		"{}", -1, "https://mcp.example/mcp", &nothing, NULL, NULL));
	g_assert_null(nothing);

	g_assert_false(clawt_mcp_auth_parse_resource(
		"[1]", -1, "https://mcp.example/mcp", NULL, NULL, NULL));
}

/* ── The authorization server's metadata ─────────────────────────── */

#define GOOD_METADATA \
	"{\"issuer\":\"https://auth.example\"," \
	"\"authorization_endpoint\":\"https://auth.example/authorize\"," \
	"\"token_endpoint\":\"https://auth.example/token\"," \
	"\"registration_endpoint\":\"https://auth.example/register\"," \
	"\"revocation_endpoint\":\"https://auth.example/revoke\"," \
	"\"code_challenge_methods_supported\":[\"S256\"]," \
	"\"token_endpoint_auth_methods_supported\":[\"none\"," \
	"\"client_secret_post\"]}"

static void
test_server_metadata_is_read(
	void
){
	g_autoptr(GError) error = NULL;
	g_autoptr(ClawtMcpAuthMetadata) metadata = NULL;

	metadata = clawt_mcp_auth_parse_server(GOOD_METADATA, -1,
	                                       "https://auth.example/",
	                                       "https://mcp.example/mcp#frag",
	                                       "read", TRUE, &error);

	g_assert_no_error(error);
	g_assert_nonnull(metadata);
	g_assert_cmpstr(metadata->authorization_endpoint, ==,
	                "https://auth.example/authorize");
	g_assert_cmpstr(metadata->token_endpoint, ==,
	                "https://auth.example/token");
	g_assert_cmpstr(metadata->registration_endpoint, ==,
	                "https://auth.example/register");
	g_assert_cmpstr(metadata->revocation_endpoint, ==,
	                "https://auth.example/revoke");
	g_assert_cmpstr(metadata->scopes, ==, "read");
	g_assert_cmpstr(metadata->source, ==, "protected-resource");
	g_assert_cmpstr(metadata->auth_method, ==, "none");

	/* The resource indicator is the server, without its fragment. */
	g_assert_cmpstr(metadata->resource, ==, "https://mcp.example/mcp");
}

/* RFC 8414 section 3.3: the mix-up attack. */
static void
test_server_metadata_for_another_issuer_is_refused(
	void
){
	g_autoptr(GError) error = NULL;
	g_autoptr(ClawtMcpAuthMetadata) metadata = NULL;

	metadata = clawt_mcp_auth_parse_server(GOOD_METADATA, -1,
	                                       "https://evil.example",
	                                       "https://mcp.example/mcp", NULL,
	                                       TRUE, &error);

	g_assert_null(metadata);
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL);
}

static void
test_server_without_s256_is_refused(
	void
){
	g_autoptr(GError) error = NULL;
	g_autoptr(ClawtMcpAuthMetadata) metadata = NULL;

	metadata = clawt_mcp_auth_parse_server(
		"{\"issuer\":\"https://auth.example\","
		"\"authorization_endpoint\":\"https://auth.example/authorize\","
		"\"token_endpoint\":\"https://auth.example/token\","
		"\"code_challenge_methods_supported\":[\"plain\"]}",
		-1, "https://auth.example", "https://mcp.example/mcp", NULL, FALSE,
		&error);

	g_assert_null(metadata);
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED);
}

/*
 * A token endpoint in the clear is where a sign-in code would be spent
 * in the clear.  Refused for the required endpoints; an optional one that
 * fails the rule is dropped rather than trusted.
 */
static void
test_server_endpoints_in_the_clear_are_refused(
	void
){
	g_autoptr(GError) error = NULL;
	g_autoptr(ClawtMcpAuthMetadata) refused = NULL;
	g_autoptr(ClawtMcpAuthMetadata) dropped = NULL;
	g_autoptr(ClawtMcpAuthMetadata) loopback = NULL;

	refused = clawt_mcp_auth_parse_server(
		"{\"issuer\":\"https://auth.example\","
		"\"authorization_endpoint\":\"https://auth.example/authorize\","
		"\"token_endpoint\":\"http://auth.example/token\","
		"\"code_challenge_methods_supported\":[\"S256\"]}",
		-1, "https://auth.example", "https://mcp.example/mcp", NULL, TRUE,
		&error);
	g_assert_null(refused);
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED);

	dropped = clawt_mcp_auth_parse_server(
		"{\"issuer\":\"https://auth.example\","
		"\"authorization_endpoint\":\"https://auth.example/authorize\","
		"\"token_endpoint\":\"https://auth.example/token\","
		"\"registration_endpoint\":\"http://127.0.0.1/register\","
		"\"code_challenge_methods_supported\":[\"S256\"],"
		"\"token_endpoint_auth_methods_supported\":[\"client_secret_basic\","
		"\"client_secret_post\"]}",
		-1, "https://auth.example", "https://mcp.example/mcp", NULL, FALSE,
		NULL);
	g_assert_nonnull(dropped);
	g_assert_null(dropped->registration_endpoint);
	g_assert_cmpstr(dropped->source, ==, "origin");
	g_assert_cmpstr(dropped->auth_method, ==, "client_secret_post");

	/* A loopback test server may sign in over plain http on loopback. */
	loopback = clawt_mcp_auth_parse_server(
		"{\"issuer\":\"http://127.0.0.1:9001\","
		"\"authorization_endpoint\":\"http://127.0.0.1:9001/authorize\","
		"\"token_endpoint\":\"http://127.0.0.1:9001/token\","
		"\"code_challenge_methods_supported\":[\"S256\"]}",
		-1, "http://127.0.0.1:9001", "http://127.0.0.1:9000/mcp", NULL, TRUE,
		NULL);
	g_assert_nonnull(loopback);
}

/* ── Registering ─────────────────────────────────────────────────── */

static void
test_registration_request_is_a_public_pkce_client(
	void
){
	g_autoptr(ClawtMcpAuthMetadata) metadata = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autofree gchar *body = NULL;
	JsonObject *object;
	JsonArray *redirects;
	JsonArray *grants;

	metadata = clawt_mcp_auth_parse_server(GOOD_METADATA, -1,
	                                       "https://auth.example",
	                                       "https://mcp.example/mcp",
	                                       "read write", TRUE, NULL);
	g_assert_nonnull(metadata);

	body = clawt_mcp_auth_registration_request(
		metadata, "http://127.0.0.1:8765/callback", "fleet-a");

	g_assert_true(json_parser_load_from_data(parser, body, -1, NULL));
	object = json_node_get_object(json_parser_get_root(parser));

	g_assert_cmpstr(json_object_get_string_member(object, "client_name"), ==,
	                "fleet-a");
	g_assert_cmpstr(json_object_get_string_member(
	                    object, "token_endpoint_auth_method"), ==, "none");
	g_assert_cmpstr(json_object_get_string_member(object, "scope"), ==,
	                "read write");

	redirects = json_object_get_array_member(object, "redirect_uris");
	g_assert_cmpuint(json_array_get_length(redirects), ==, 1);
	g_assert_cmpstr(json_array_get_string_element(redirects, 0), ==,
	                "http://127.0.0.1:8765/callback");

	grants = json_object_get_array_member(object, "grant_types");
	g_assert_cmpstr(json_array_get_string_element(grants, 0), ==,
	                "authorization_code");
	g_assert_cmpstr(json_array_get_string_element(grants, 1), ==,
	                "refresh_token");

	/* Nothing secret is ever sent to be registered. */
	g_assert_false(json_object_has_member(object, "client_secret"));
}

static void
test_registration_response_is_read(
	void
){
	g_autoptr(GError) error = NULL;
	g_autoptr(ClawtMcpAuthClient) client = NULL;
	g_autoptr(ClawtMcpAuthClient) refused = NULL;
	g_autoptr(ClawtMcpAuthClient) empty = NULL;

	client = clawt_mcp_auth_parse_registration(
		"{\"client_id\":\"c-123\",\"client_secret\":\"s-456\","
		"\"client_id_issued_at\":1}", -1, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(client->client_id, ==, "c-123");
	g_assert_cmpstr(client->client_secret, ==, "s-456");

	/* The server's own words, which name what to fix. */
	refused = clawt_mcp_auth_parse_registration(
		"{\"error\":\"invalid_redirect_uri\","
		"\"error_description\":\"loopback not allowed\"}", -1, &error);
	g_assert_null(refused);
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_AUTH);
	g_assert_nonnull(strstr(error->message, "invalid_redirect_uri"));
	g_assert_nonnull(strstr(error->message, "loopback not allowed"));
	g_clear_error(&error);

	empty = clawt_mcp_auth_parse_registration("{}", -1, &error);
	g_assert_null(empty);
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL);
}

/* ── Deciding ────────────────────────────────────────────────────── */

static void
test_plan_decisions(
	void
){
	ClawtMcpAuthMetadata with_registration = { 0 };
	ClawtMcpAuthMetadata without_registration = { 0 };

	with_registration.registration_endpoint = (gchar *)"https://a/register";

	/* A pasted key is not a flow at all. */
	g_assert_cmpint(clawt_mcp_auth_plan(FALSE, FALSE, TRUE, NULL, NULL), ==,
	                CLAWT_MCP_AUTH_PLAN_NONE);

	/* Hand-configured endpoints win over anything a server publishes. */
	g_assert_cmpint(clawt_mcp_auth_plan(TRUE, TRUE, TRUE,
	                                    &with_registration, NULL), ==,
	                CLAWT_MCP_AUTH_PLAN_CONFIGURED);

	/* A flow with nothing to discover from. */
	g_assert_cmpint(clawt_mcp_auth_plan(TRUE, FALSE, FALSE, NULL, NULL), ==,
	                CLAWT_MCP_AUTH_PLAN_NONE);

	g_assert_cmpint(clawt_mcp_auth_plan(TRUE, FALSE, TRUE, NULL, "cid"), ==,
	                CLAWT_MCP_AUTH_PLAN_DISCOVER);

	/* A configured client id wins over registering one. */
	g_assert_cmpint(clawt_mcp_auth_plan(TRUE, FALSE, TRUE,
	                                    &with_registration, "cid"), ==,
	                CLAWT_MCP_AUTH_PLAN_SIGN_IN);
	g_assert_cmpint(clawt_mcp_auth_plan(TRUE, FALSE, TRUE,
	                                    &with_registration, NULL), ==,
	                CLAWT_MCP_AUTH_PLAN_REGISTER);
	g_assert_cmpint(clawt_mcp_auth_plan(TRUE, FALSE, TRUE,
	                                    &with_registration, ""), ==,
	                CLAWT_MCP_AUTH_PLAN_REGISTER);
	g_assert_cmpint(clawt_mcp_auth_plan(TRUE, FALSE, TRUE,
	                                    &without_registration, NULL), ==,
	                CLAWT_MCP_AUTH_PLAN_NO_CLIENT);
}

/* ── The resource parameter, on every leg ────────────────────────── */

static void
test_resource_rides_every_leg(
	void
){
	g_autofree gchar *url = NULL;
	g_autofree gchar *exchange = NULL;
	g_autofree gchar *refresh = NULL;
	g_autofree gchar *plain = NULL;
	const gchar *escaped = "resource=https%3A%2F%2Fmcp.example%2Fmcp";

	url = clawt_oauth_authorize_url("https://auth.example/authorize", "cid",
	                                "http://127.0.0.1:8765/callback", "read",
	                                "st", "ch", "https://mcp.example/mcp");
	exchange = clawt_oauth_exchange_body("cid", NULL, "code-1",
	                                     "http://127.0.0.1:8765/callback",
	                                     "verifier",
	                                     "https://mcp.example/mcp");
	refresh = clawt_oauth_refresh_body("cid", NULL, "r-1",
	                                   "https://mcp.example/mcp");

	g_assert_nonnull(strstr(url, escaped));
	g_assert_nonnull(strstr(exchange, escaped));
	g_assert_nonnull(strstr(exchange, "code_verifier=verifier"));
	g_assert_nonnull(strstr(refresh, escaped));
	g_assert_nonnull(strstr(refresh, "grant_type=refresh_token"));

	/* And a configured connector sends none, rather than an empty one. */
	plain = clawt_oauth_refresh_body("cid", NULL, "r-1", NULL);
	g_assert_null(strstr(plain, "resource"));
}

/* ── Redirects ───────────────────────────────────────────────────── */

/*
 * Every discovery leg goes out on a message that refuses redirects, and
 * a 3xx reply becomes an error naming where it pointed rather than a
 * fetch of whatever host the server chose.
 */
static void
test_discovery_never_follows_a_redirect(
	void
){
	g_autoptr(SoupMessage) get = NULL;
	g_autoptr(SoupMessage) post = NULL;
	g_autoptr(GError) refusal = NULL;

	get = clawt_mcp_auth_document_message_new(
		"GET", "https://mcp.example/.well-known/oauth-protected-resource");
	post = clawt_mcp_auth_document_message_new("POST",
	                                           "https://auth.example/register");

	g_assert_nonnull(get);
	g_assert_nonnull(post);
	g_assert_true(soup_message_query_flags(get, SOUP_MESSAGE_NO_REDIRECT));
	g_assert_true(soup_message_query_flags(post, SOUP_MESSAGE_NO_REDIRECT));
	g_assert_cmpstr(soup_message_headers_get_one(
	                    soup_message_get_request_headers(get), "Accept"), ==,
	                "application/json");

	refusal = clawt_http_redirect_refusal(
		"https://auth.example/register", 307,
		"https://elsewhere.example/register");
	g_assert_error(refusal, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED);
	g_assert_nonnull(strstr(refusal->message, "elsewhere.example"));
}

/* ── The 401 the agent hears ─────────────────────────────────────── */

static JsonObject *
parse(
	JsonParser  *parser,
	const gchar *text
){
	g_assert_true(json_parser_load_from_data(parser, text, -1, NULL));

	return json_node_get_object(json_parser_get_root(parser));
}

/*
 * The case 608ff19fb fixed elsewhere: a server that lists tools to
 * anybody and refuses only tool calls.  The refused call must come back
 * as a tool result the model reads, naming the connector and the
 * sign-in -- not a protocol error a CLI reports as "server failed".
 */
static void
test_a_refused_tool_call_is_actionable(
	void
){
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autofree gchar *reply = NULL;
	JsonObject *object;
	JsonObject *result;
	JsonArray *content;
	const gchar *text;

	reply = clawt_mcp_auth_unauthorized_reply(
		"{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\","
		"\"params\":{\"name\":\"search\"}}", "notion", FALSE);

	g_assert_nonnull(reply);
	object = parse(parser, reply);

	g_assert_cmpint(json_object_get_int_member(object, "id"), ==, 7);
	g_assert_false(json_object_has_member(object, "error"));

	result = json_object_get_object_member(object, "result");
	g_assert_true(json_object_get_boolean_member(result, "isError"));

	content = json_object_get_array_member(result, "content");
	text = json_object_get_string_member(
		json_array_get_object_element(content, 0), "text");

	g_assert_nonnull(strstr(text, "'notion'"));
	g_assert_nonnull(strstr(text, "401"));
	g_assert_nonnull(strstr(text, "clawtilla connector connect notion"));
	g_assert_nonnull(strstr(text, "Sign in"));
	g_assert_nonnull(strstr(text, "Do not retry"));
}

static void
test_other_refused_requests_are_errors(
	void
){
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autofree gchar *listed = NULL;
	g_autofree gchar *expired = NULL;
	g_autofree gchar *notified = NULL;
	JsonObject *error;

	listed = clawt_mcp_auth_unauthorized_reply(
		"{\"jsonrpc\":\"2.0\",\"id\":\"a\",\"method\":\"tools/list\"}",
		"notion", FALSE);
	error = json_object_get_object_member(parse(parser, listed), "error");
	g_assert_nonnull(strstr(json_object_get_string_member(error, "message"),
	                        "needs a sign-in"));

	/* A credential that was sent and refused says so differently. */
	expired = clawt_mcp_auth_unauthorized_reply(
		"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\"}",
		"notion", TRUE);
	g_assert_nonnull(strstr(expired, "refused the credential"));

	/* A notification has nothing to answer into. */
	notified = clawt_mcp_auth_unauthorized_reply(
		"{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}",
		"notion", FALSE);
	g_assert_null(notified);
}

/* ── The auth file ───────────────────────────────────────────────── */

static void
test_auth_file_round_trips_at_0600(
	void
){
	g_autofree gchar *dir = g_dir_make_tmp("clawt-mcp-auth-XXXXXX", NULL);
	g_autofree gchar *path = clawt_mcp_auth_path(dir, "notion");
	g_autofree gchar *awkward = clawt_mcp_auth_path(dir, "../../etc/x");
	g_autofree gchar *token_like = clawt_connector_token_path(dir, "x-auth");
	g_autoptr(ClawtMcpAuthMetadata) metadata = NULL;
	g_autoptr(ClawtMcpAuthMetadata) loaded = NULL;
	g_autoptr(ClawtMcpAuthClient) client = NULL;
	g_autoptr(ClawtMcpAuthClient) loaded_client = NULL;
	g_autoptr(ClawtMcpAuthMetadata) missing = NULL;
	g_autoptr(GError) error = NULL;
	GStatBuf st;

	/* A name from a config file cannot climb out of the directory. */
	g_assert_true(g_str_has_prefix(awkward, dir));
	g_assert_null(strstr(awkward + strlen(dir), "/../"));
	g_assert_cmpstr(path, !=, token_like);

	g_assert_true(clawt_mcp_auth_load(path, &missing, NULL, &error));
	g_assert_no_error(error);
	g_assert_null(missing);

	metadata = clawt_mcp_auth_parse_server(GOOD_METADATA, -1,
	                                       "https://auth.example",
	                                       "https://mcp.example/mcp", NULL,
	                                       TRUE, NULL);
	client = g_new0(ClawtMcpAuthClient, 1);
	client->client_id = g_strdup("c-1");
	client->client_secret = g_strdup("s-1");

	g_assert_true(clawt_mcp_auth_save(path, metadata, client, &error));
	g_assert_no_error(error);

	g_assert_cmpint(g_stat(path, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0600);

	g_assert_true(clawt_mcp_auth_load(path, &loaded, &loaded_client, &error));
	g_assert_cmpstr(loaded->issuer, ==, "https://auth.example");
	g_assert_cmpstr(loaded->token_endpoint, ==, "https://auth.example/token");
	g_assert_cmpstr(loaded->resource, ==, "https://mcp.example/mcp");
	g_assert_cmpstr(loaded_client->client_id, ==, "c-1");
	g_assert_cmpstr(loaded_client->client_secret, ==, "s-1");

	clawt_test_remove_tree(dir);
}

/* ── Adding by URL alone ─────────────────────────────────────────── */

static void
test_a_name_is_proposed_from_the_url(
	void
){
	g_autofree gchar *notion = clawt_connector_name_for_url(
		"https://mcp.notion.com/mcp");
	g_autofree gchar *linear = clawt_connector_name_for_url(
		"https://mcp.linear.app/sse");
	g_autofree gchar *local = clawt_connector_name_for_url(
		"http://localhost:9000/mcp");
	g_autofree gchar *address = clawt_connector_name_for_url(
		"http://127.0.0.1:9000/mcp");
	g_autofree gchar *nothing = clawt_connector_name_for_url("nonsense");

	g_assert_cmpstr(notion, ==, "notion");
	g_assert_cmpstr(linear, ==, "linear");
	g_assert_cmpstr(local, ==, "localhost");
	g_assert_cmpstr(address, ==, CLAWT_CONNECTOR_PROVIDER_REMOTE);
	g_assert_null(nothing);
}

/*
 * The remote entry is first in the catalogue, so a client that opens its
 * list on the first entry opens on "any server by URL" without knowing
 * that entry's id.
 */
static void
test_the_remote_entry_sorts_first(
	void
){
	g_autoptr(GPtrArray) catalog = clawt_connector_catalog_load(NULL, NULL);
	const ClawtConnectorInfo *first;

	g_assert_cmpuint(catalog->len, >, 1);
	first = g_ptr_array_index(catalog, 0);
	g_assert_cmpstr(first->id, ==, CLAWT_CONNECTOR_PROVIDER_REMOTE);
	g_assert_cmpint(first->auth, ==, CLAWT_CONNECTOR_AUTH_PKCE);
	g_assert_null(first->auth_url);
	g_assert_null(first->token_url);
}

/*
 * A server that offers only client_secret_basic refuses a secret in the
 * body; one that offers post or public clients gets those instead.
 */
static void
test_token_endpoint_auth_method_is_honoured(
	void
){
	g_autoptr(ClawtMcpAuthMetadata) basic_only = NULL;
	g_autoptr(ClawtMcpAuthMetadata) public_ok = NULL;
	g_autoptr(ClawtMcpAuthClient) granted = NULL;
	g_autofree gchar *header = NULL;
	g_autofree guchar *raw = NULL;
	g_autofree gchar *decoded = NULL;
	gsize length = 0;

	basic_only = clawt_mcp_auth_parse_server(
		"{\"issuer\":\"https://auth.example\","
		"\"authorization_endpoint\":\"https://auth.example/authorize\","
		"\"token_endpoint\":\"https://auth.example/token\","
		"\"code_challenge_methods_supported\":[\"S256\"],"
		"\"token_endpoint_auth_methods_supported\":"
		"[\"client_secret_basic\"]}",
		-1, "https://auth.example", "https://mcp.example/mcp", NULL, TRUE,
		NULL);
	g_assert_cmpstr(basic_only->auth_method, ==, "client_secret_basic");
	g_assert_true(clawt_mcp_auth_uses_basic(basic_only, NULL));

	public_ok = clawt_mcp_auth_parse_server(GOOD_METADATA, -1,
	                                        "https://auth.example",
	                                        "https://mcp.example/mcp", NULL,
	                                        TRUE, NULL);
	g_assert_false(clawt_mcp_auth_uses_basic(public_ok, NULL));

	/* What the registration granted wins over what was offered. */
	granted = clawt_mcp_auth_parse_registration(
		"{\"client_id\":\"c\",\"client_secret\":\"s\","
		"\"token_endpoint_auth_method\":\"client_secret_basic\"}", -1, NULL);
	g_assert_true(clawt_mcp_auth_uses_basic(public_ok, granted));

	/* Both halves form-encoded before they are joined (RFC 6749 2.3.1). */
	header = clawt_oauth_basic_credentials("my id", "a:b+c");
	g_assert_true(g_str_has_prefix(header, "Basic "));
	raw = g_base64_decode(header + 6, &length);
	decoded = g_strndup((const gchar *)raw, length);
	g_assert_cmpstr(decoded, ==, "my%20id:a%3Ab%2Bc");
}

int
main(
	int   argc,
	char *argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/mcp-auth/challenge/quoted-or-bare",
	                test_challenge_is_read_quoted_or_bare);
	g_test_add_func("/mcp-auth/challenge/name-inside-a-value",
	                test_challenge_ignores_the_name_inside_a_value);
	g_test_add_func("/mcp-auth/url/https-only",
	                test_https_is_allowed_and_plain_http_is_not);
	g_test_add_func("/mcp-auth/url/loopback-only-from-loopback",
	                test_loopback_only_from_loopback);
	g_test_add_func("/mcp-auth/candidates/resource",
	                test_resource_candidates_follow_rfc_9728);
	g_test_add_func("/mcp-auth/candidates/server",
	                test_server_candidates_follow_rfc_8414);
	g_test_add_func("/mcp-auth/resource/names-the-server",
	                test_resource_document_names_the_server);
	g_test_add_func("/mcp-auth/resource/another-server-refused",
	                test_resource_document_for_another_server_is_refused);
	g_test_add_func("/mcp-auth/resource/origin-accepted",
	                test_resource_document_at_the_origin_is_accepted);
	g_test_add_func("/mcp-auth/server/read", test_server_metadata_is_read);
	g_test_add_func("/mcp-auth/server/mix-up-refused",
	                test_server_metadata_for_another_issuer_is_refused);
	g_test_add_func("/mcp-auth/server/no-s256-refused",
	                test_server_without_s256_is_refused);
	g_test_add_func("/mcp-auth/server/cleartext-refused",
	                test_server_endpoints_in_the_clear_are_refused);
	g_test_add_func("/mcp-auth/register/request",
	                test_registration_request_is_a_public_pkce_client);
	g_test_add_func("/mcp-auth/register/response",
	                test_registration_response_is_read);
	g_test_add_func("/mcp-auth/plan", test_plan_decisions);
	g_test_add_func("/mcp-auth/token-endpoint-auth-method",
	                test_token_endpoint_auth_method_is_honoured);
	g_test_add_func("/mcp-auth/resource-parameter",
	                test_resource_rides_every_leg);
	g_test_add_func("/mcp-auth/no-redirects",
	                test_discovery_never_follows_a_redirect);
	g_test_add_func("/mcp-auth/401/tool-call-actionable",
	                test_a_refused_tool_call_is_actionable);
	g_test_add_func("/mcp-auth/401/other-requests",
	                test_other_refused_requests_are_errors);
	g_test_add_func("/mcp-auth/file/round-trip",
	                test_auth_file_round_trips_at_0600);
	g_test_add_func("/mcp-auth/add/name-from-url",
	                test_a_name_is_proposed_from_the_url);
	g_test_add_func("/mcp-auth/add/remote-first",
	                test_the_remote_entry_sorts_first);

	return g_test_run();
}
