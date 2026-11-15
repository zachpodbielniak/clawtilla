/*
 * clawt-mcp-auth.c - Finding out how to sign in to a remote MCP server
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "clawt-http.h"
#include "integration/clawt-mcp-auth.h"

#include <string.h>

#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

#define USER_AGENT "clawtilla/" CLAWT_VERSION_STRING

/* Discovery is a person waiting on a dialog; a server that hangs is a failure. */
#define DISCOVERY_TIMEOUT_SECONDS (15)

/* JSON-RPC's implementation-defined server error range. */
#define JSONRPC_UNAUTHORIZED (-32001)

/* ── Boxed types ─────────────────────────────────────────────────── */

ClawtMcpAuthMetadata *
clawt_mcp_auth_metadata_copy(
	ClawtMcpAuthMetadata *self
){
	ClawtMcpAuthMetadata *out;

	g_return_val_if_fail(self != NULL, NULL);

	out = g_new0(ClawtMcpAuthMetadata, 1);
	out->issuer = g_strdup(self->issuer);
	out->authorization_endpoint = g_strdup(self->authorization_endpoint);
	out->token_endpoint = g_strdup(self->token_endpoint);
	out->registration_endpoint = g_strdup(self->registration_endpoint);
	out->revocation_endpoint = g_strdup(self->revocation_endpoint);
	out->scopes = g_strdup(self->scopes);
	out->resource = g_strdup(self->resource);
	out->source = g_strdup(self->source);
	out->auth_method = g_strdup(self->auth_method);

	return out;
}

void
clawt_mcp_auth_metadata_free(
	ClawtMcpAuthMetadata *self
){
	if (self == NULL)
		return;

	g_free(self->issuer);
	g_free(self->authorization_endpoint);
	g_free(self->token_endpoint);
	g_free(self->registration_endpoint);
	g_free(self->revocation_endpoint);
	g_free(self->scopes);
	g_free(self->resource);
	g_free(self->source);
	g_free(self->auth_method);
	g_free(self);
}

G_DEFINE_BOXED_TYPE(ClawtMcpAuthMetadata, clawt_mcp_auth_metadata,
                    clawt_mcp_auth_metadata_copy,
                    clawt_mcp_auth_metadata_free)

ClawtMcpAuthClient *
clawt_mcp_auth_client_copy(
	ClawtMcpAuthClient *self
){
	ClawtMcpAuthClient *out;

	g_return_val_if_fail(self != NULL, NULL);

	out = g_new0(ClawtMcpAuthClient, 1);
	out->client_id = g_strdup(self->client_id);
	out->client_secret = g_strdup(self->client_secret);
	out->auth_method = g_strdup(self->auth_method);

	return out;
}

/* A freed secret is heap handed straight back out; wipe it first. */
static void
wipe(
	gchar *secret
){
	if (secret == NULL)
		return;

	memset((void *volatile)secret, 0, strlen(secret));
}

void
clawt_mcp_auth_client_free(
	ClawtMcpAuthClient *self
){
	if (self == NULL)
		return;

	wipe(self->client_secret);
	g_free(self->client_id);
	g_free(self->client_secret);
	g_free(self->auth_method);
	g_free(self);
}

G_DEFINE_BOXED_TYPE(ClawtMcpAuthClient, clawt_mcp_auth_client,
                    clawt_mcp_auth_client_copy,
                    clawt_mcp_auth_client_free)

/* ── URLs ────────────────────────────────────────────────────────── */

/*
 * Whether a host names this machine.
 *
 * Asked of GInetAddress rather than of a string comparison, because
 * 127.0.0.2, ::1 and ::ffff:127.0.0.1 are all loopback and a list of
 * spellings misses whichever one a hostile document picks.
 */
static gboolean
host_is_loopback(
	const gchar *host
){
	g_autoptr(GInetAddress) address = NULL;
	g_autofree gchar *bare = NULL;
	gsize length;

	if (host == NULL || *host == '\0')
		return FALSE;

	bare = g_ascii_strdown(host, -1);
	length = strlen(bare);

	/* A trailing dot is the same name, fully qualified. */
	if (length > 0 && bare[length - 1] == '.')
		bare[length - 1] = '\0';

	if (g_strcmp0(bare, "localhost") == 0 ||
	    g_str_has_suffix(bare, ".localhost"))
		return TRUE;

	address = g_inet_address_new_from_string(bare);

	if (address == NULL)
		return FALSE;

	if (g_inet_address_get_is_loopback(address) ||
	    g_inet_address_get_is_any(address))
		return TRUE;

	/* ::ffff:127.0.0.1 -- an IPv4 loopback dressed as IPv6. */
	if (g_inet_address_get_family(address) == G_SOCKET_FAMILY_IPV6 &&
	    g_str_has_prefix(bare, "::ffff:127."))
		return TRUE;

	return FALSE;
}

/* scheme://host[:port], with an IPv6 literal bracketed again. */
static gchar *
uri_origin(
	GUri *uri
){
	const gchar *host = g_uri_get_host(uri);
	gint port = g_uri_get_port(uri);
	g_autofree gchar *scheme = g_ascii_strdown(g_uri_get_scheme(uri), -1);
	gboolean v6 = (host != NULL && strchr(host, ':') != NULL);

	if (port > 0)
		return g_strdup_printf("%s://%s%s%s:%d", scheme, v6 ? "[" : "",
		                       host, v6 ? "]" : "", port);

	return g_strdup_printf("%s://%s%s%s", scheme, v6 ? "[" : "", host,
	                       v6 ? "]" : "");
}

static GUri *
parse_web_url(
	const gchar *url
){
	g_autoptr(GUri) uri = NULL;
	const gchar *scheme;

	if (url == NULL)
		return NULL;

	uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);

	if (uri == NULL)
		return NULL;

	scheme = g_uri_get_scheme(uri);

	if (g_ascii_strcasecmp(scheme, "https") != 0 &&
	    g_ascii_strcasecmp(scheme, "http") != 0)
		return NULL;

	if (g_uri_get_host(uri) == NULL || *g_uri_get_host(uri) == '\0')
		return NULL;

	return g_steal_pointer(&uri);
}

gboolean
clawt_mcp_auth_url_allowed(
	const gchar *url,
	const gchar *trusted_url
){
	g_autoptr(GUri) uri = NULL;
	g_autoptr(GUri) trusted = NULL;
	gboolean loopback;

	uri = parse_web_url(url);
	trusted = parse_web_url(trusted_url);

	if (uri == NULL || trusted == NULL)
		return FALSE;

	/*
	 * Credentials in a URL end up in logs and in front of a person; a
	 * metadata document has no business carrying any.
	 */
	if (g_uri_get_userinfo(uri) != NULL)
		return FALSE;

	loopback = host_is_loopback(g_uri_get_host(uri));

	/*
	 * Loopback only from loopback.  A server the operator started on
	 * this machine may name a sign-in server on another local port; a
	 * server anywhere else naming one is aiming the daemon at this
	 * machine's own unauthenticated services.
	 */
	if (loopback)
		return host_is_loopback(g_uri_get_host(trusted));

	return g_ascii_strcasecmp(g_uri_get_scheme(uri), "https") == 0;
}

/* The URL as an identifier: no fragment, no trailing slash. */
static gchar *
normalise_resource(
	const gchar *url
){
	g_autoptr(GUri) uri = NULL;
	g_autoptr(GUri) bare = NULL;
	gchar *out;
	gsize length;

	uri = parse_web_url(url);

	if (uri == NULL)
		return g_strdup(url);

	bare = g_uri_build(G_URI_FLAGS_NONE, g_uri_get_scheme(uri), NULL,
	                   g_uri_get_host(uri), g_uri_get_port(uri),
	                   g_uri_get_path(uri), g_uri_get_query(uri), NULL);
	out = g_uri_to_string(bare);
	length = strlen(out);

	while (length > 0 && out[length - 1] == '/') {
		out[length - 1] = '\0';
		length--;
	}

	return out;
}

/* ── Reading documents ───────────────────────────────────────────── */

gchar *
clawt_mcp_auth_parse_challenge(
	const gchar *www_authenticate
){
	const gchar *p;

	if (www_authenticate == NULL)
		return NULL;

	/*
	 * Walked parameter by parameter rather than searched for the name
	 * anywhere, so a realm that happens to contain the words
	 * "resource_metadata=" is not read as the parameter.
	 */
	p = www_authenticate;

	while (*p != '\0') {
		const gchar *name;
		gsize name_length;

		while (*p == ' ' || *p == '\t' || *p == ',')
			p++;

		name = p;

		while (*p != '\0' && *p != '=' && *p != ' ' && *p != ',' &&
		       *p != '\t')
			p++;

		name_length = (gsize)(p - name);

		/* An auth scheme ("Bearer") is a bare word with no '='. */
		while (*p == ' ' || *p == '\t')
			p++;

		if (*p != '=')
			continue;

		p++;

		while (*p == ' ' || *p == '\t')
			p++;

		if (*p == '"') {
			GString *value = g_string_new(NULL);

			p++;

			while (*p != '\0' && *p != '"') {
				if (*p == '\\' && p[1] != '\0')
					p++;

				g_string_append_c(value, *p);
				p++;
			}

			if (*p == '"')
				p++;

			if (name_length == strlen("resource_metadata") &&
			    g_ascii_strncasecmp(name, "resource_metadata",
			                        name_length) == 0 &&
			    value->len > 0)
				return g_string_free(value, FALSE);

			g_string_free(value, TRUE);
		} else {
			const gchar *start = p;

			while (*p != '\0' && *p != ',' && *p != ' ' && *p != '\t')
				p++;

			if (name_length == strlen("resource_metadata") &&
			    g_ascii_strncasecmp(name, "resource_metadata",
			                        name_length) == 0 &&
			    p > start)
				return g_strndup(start, (gsize)(p - start));
		}
	}

	return NULL;
}

/* Appends @url to @list unless it is already there. */
static void
add_unique(
	GPtrArray   *list,
	const gchar *url
){
	guint i;

	for (i = 0; i < list->len; i++)
		if (g_strcmp0(g_ptr_array_index(list, i), url) == 0)
			return;

	g_ptr_array_add(list, g_strdup(url));
}

GStrv
clawt_mcp_auth_resource_candidates(
	const gchar *mcp_url,
	const gchar *hint
){
	g_autoptr(GPtrArray) list = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GUri) uri = NULL;

	if (hint != NULL && *hint != '\0')
		add_unique(list, hint);

	uri = parse_web_url(mcp_url);

	if (uri != NULL) {
		g_autofree gchar *origin = uri_origin(uri);
		const gchar *path = g_uri_get_path(uri);
		g_autofree gchar *at_path = NULL;
		g_autofree gchar *at_root = NULL;

		/* RFC 9728 section 3.1: the well-known part goes before the path. */
		if (path != NULL && *path != '\0' && g_strcmp0(path, "/") != 0) {
			at_path = g_strconcat(origin,
			                      "/.well-known/oauth-protected-resource",
			                      path, NULL);
			add_unique(list, at_path);
		}

		at_root = g_strconcat(origin, "/.well-known/oauth-protected-resource",
		                      NULL);
		add_unique(list, at_root);
	}

	g_ptr_array_add(list, NULL);

	return (GStrv)g_ptr_array_free(g_steal_pointer(&list), FALSE);
}

GStrv
clawt_mcp_auth_server_candidates(
	const gchar *issuer
){
	g_autoptr(GPtrArray) list = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GUri) uri = NULL;

	uri = parse_web_url(issuer);

	if (uri != NULL) {
		g_autofree gchar *origin = uri_origin(uri);
		g_autofree gchar *path = g_strdup(g_uri_get_path(uri));
		g_autofree gchar *oauth = NULL;
		g_autofree gchar *oidc = NULL;
		gsize length = strlen(path);

		while (length > 0 && path[length - 1] == '/') {
			path[length - 1] = '\0';
			length--;
		}

		oauth = g_strconcat(origin, "/.well-known/oauth-authorization-server",
		                    path, NULL);
		oidc = g_strconcat(origin, "/.well-known/openid-configuration", path,
		                   NULL);
		add_unique(list, oauth);
		add_unique(list, oidc);

		/* OpenID Connect's own rule appends instead of inserting. */
		if (*path != '\0') {
			g_autofree gchar *appended = g_strconcat(
				origin, path, "/.well-known/openid-configuration", NULL);

			add_unique(list, appended);
		}
	}

	g_ptr_array_add(list, NULL);

	return (GStrv)g_ptr_array_free(g_steal_pointer(&list), FALSE);
}

static JsonObject *
parse_object(
	JsonParser   *parser,
	const gchar  *json,
	gssize        length,
	const gchar  *what,
	GError      **error
){
	JsonNode *root;

	if (json == NULL ||
	    !json_parser_load_from_data(parser, json, length, NULL)) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
		            "the %s is not JSON", what);
		return NULL;
	}

	root = json_parser_get_root(parser);

	if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root) ||
	    json_node_get_object(root) == NULL) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
		            "the %s is not a JSON object", what);
		return NULL;
	}

	return json_node_get_object(root);
}

/* A string member, or %NULL for absent, empty or the wrong type. */
static const gchar *
member_string(
	JsonObject  *object,
	const gchar *name
){
	JsonNode *node;
	const gchar *value;

	if (!json_object_has_member(object, name))
		return NULL;

	node = json_object_get_member(object, name);

	if (!JSON_NODE_HOLDS_VALUE(node) ||
	    json_node_get_value_type(node) != G_TYPE_STRING)
		return NULL;

	value = json_node_get_string(node);

	return (value != NULL && *value != '\0') ? value : NULL;
}

/* The string elements of an array member, or %NULL. */
static GStrv
member_strings(
	JsonObject  *object,
	const gchar *name
){
	g_autoptr(GPtrArray) out = NULL;
	JsonArray *array;
	guint i;

	if (!json_object_has_member(object, name) ||
	    !JSON_NODE_HOLDS_ARRAY(json_object_get_member(object, name)))
		return NULL;

	array = json_object_get_array_member(object, name);
	out = g_ptr_array_new_with_free_func(g_free);

	for (i = 0; i < json_array_get_length(array); i++) {
		JsonNode *node = json_array_get_element(array, i);

		if (JSON_NODE_HOLDS_VALUE(node) &&
		    json_node_get_value_type(node) == G_TYPE_STRING)
			g_ptr_array_add(out, g_strdup(json_node_get_string(node)));
	}

	g_ptr_array_add(out, NULL);

	return (GStrv)g_ptr_array_free(g_steal_pointer(&out), FALSE);
}

gboolean
clawt_mcp_auth_uses_basic(
	const ClawtMcpAuthMetadata *metadata,
	const ClawtMcpAuthClient   *client
){
	const gchar *method = NULL;

	if (client != NULL && client->auth_method != NULL)
		method = client->auth_method;
	else if (metadata != NULL)
		method = metadata->auth_method;

	return g_strcmp0(method, "client_secret_basic") == 0;
}

gboolean
clawt_mcp_auth_parse_resource(
	const gchar  *json,
	gssize        length,
	const gchar  *mcp_url,
	gchar       **out_issuer,
	gchar       **out_scopes,
	GError      **error
){
	g_autoptr(JsonParser) parser = json_parser_new();
	g_auto(GStrv) servers = NULL;
	g_auto(GStrv) scopes = NULL;
	JsonObject *object;
	const gchar *resource;

	g_return_val_if_fail(mcp_url != NULL, FALSE);

	if (out_issuer != NULL)
		*out_issuer = NULL;

	if (out_scopes != NULL)
		*out_scopes = NULL;

	object = parse_object(parser, json, length,
	                      "protected-resource document", error);

	if (object == NULL)
		return FALSE;

	/*
	 * The document must be about this server, or about its origin when
	 * it was the origin's well-known address that answered.  Absent is
	 * tolerated -- it is required, and several servers in the wild leave
	 * it out -- but present and different is refused.
	 */
	resource = member_string(object, "resource");

	if (resource != NULL) {
		g_autofree gchar *mine = normalise_resource(mcp_url);
		g_autofree gchar *theirs = normalise_resource(resource);
		g_autoptr(GUri) uri = parse_web_url(mcp_url);
		g_autofree gchar *origin = (uri != NULL) ? uri_origin(uri) : NULL;

		if (g_strcmp0(mine, theirs) != 0 &&
		    g_strcmp0(origin, theirs) != 0) {
			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
			            "the protected-resource document describes %s, "
			            "not %s", resource, mcp_url);
			return FALSE;
		}
	}

	servers = member_strings(object, "authorization_servers");

	if (out_issuer != NULL && servers != NULL && servers[0] != NULL &&
	    *servers[0] != '\0')
		*out_issuer = g_strdup(servers[0]);

	scopes = member_strings(object, "scopes_supported");

	if (out_scopes != NULL && scopes != NULL && scopes[0] != NULL)
		*out_scopes = g_strjoinv(" ", scopes);

	return TRUE;
}

static gboolean
strv_has(
	GStrv        list,
	const gchar *value
){
	return list != NULL && g_strv_contains((const gchar * const *)list,
	                                       value);
}

ClawtMcpAuthMetadata *
clawt_mcp_auth_parse_server(
	const gchar  *json,
	gssize        length,
	const gchar  *issuer,
	const gchar  *mcp_url,
	const gchar  *scopes,
	gboolean      named,
	GError      **error
){
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(ClawtMcpAuthMetadata) out = NULL;
	g_auto(GStrv) methods = NULL;
	g_auto(GStrv) auth_methods = NULL;
	JsonObject *object;
	const gchar *stated;
	const gchar *authorize;
	const gchar *token;
	const gchar *registration;
	const gchar *revocation;

	g_return_val_if_fail(issuer != NULL, NULL);
	g_return_val_if_fail(mcp_url != NULL, NULL);

	object = parse_object(parser, json, length,
	                      "authorization server's metadata", error);

	if (object == NULL)
		return NULL;

	/*
	 * RFC 8414 section 3.3: metadata for some other issuer is how a
	 * mix-up attack sends a code to the wrong server.  Compared without
	 * a trailing slash, which servers add and drop inconsistently.
	 */
	stated = member_string(object, "issuer");

	if (stated != NULL) {
		g_autofree gchar *a = normalise_resource(stated);
		g_autofree gchar *b = normalise_resource(issuer);

		if (g_strcmp0(a, b) != 0) {
			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
			            "the metadata fetched for %s says it is %s's",
			            issuer, stated);
			return NULL;
		}
	}

	authorize = member_string(object, "authorization_endpoint");
	token = member_string(object, "token_endpoint");

	if (authorize == NULL || token == NULL) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
		            "%s names no %s", issuer,
		            authorize == NULL ? "authorization endpoint"
		                              : "token endpoint");
		return NULL;
	}

	if (!clawt_mcp_auth_url_allowed(authorize, mcp_url) ||
	    !clawt_mcp_auth_url_allowed(token, mcp_url)) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED,
		            "%s names an endpoint that is not https (only a "
		            "loopback test server may use plain http)", issuer);
		return NULL;
	}

	/*
	 * PKCE with S256 or nothing.  The MCP authorization spec makes it a
	 * client's duty to refuse a server that does not advertise it, and
	 * without it an authorization code intercepted on its way back to
	 * the loopback listener is a code anybody can spend.
	 */
	methods = member_strings(object, "code_challenge_methods_supported");

	if (!strv_has(methods, "S256")) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
		            "%s does not offer PKCE with S256, so clawtilla will "
		            "not sign in there", issuer);
		return NULL;
	}

	out = g_new0(ClawtMcpAuthMetadata, 1);
	out->issuer = g_strdup(stated != NULL ? stated : issuer);
	out->authorization_endpoint = g_strdup(authorize);
	out->token_endpoint = g_strdup(token);

	/* Optional endpoints that fail the rule are dropped, not trusted. */
	registration = member_string(object, "registration_endpoint");

	if (registration != NULL &&
	    clawt_mcp_auth_url_allowed(registration, mcp_url))
		out->registration_endpoint = g_strdup(registration);

	revocation = member_string(object, "revocation_endpoint");

	if (revocation != NULL && clawt_mcp_auth_url_allowed(revocation, mcp_url))
		out->revocation_endpoint = g_strdup(revocation);

	out->scopes = g_strdup(scopes);
	out->resource = normalise_resource(mcp_url);
	out->source = g_strdup(named ? "protected-resource" : "origin");

	/*
	 * A public client when the server takes one.  Absent means the
	 * RFC 8414 default, client_secret_basic, which a public client is
	 * not -- but a server that lists nothing almost always accepts
	 * "none" from a registration that asks, so asking is right.  A server
	 * that takes only a secret gets the method it names: in the body when
	 * it offers that, as HTTP Basic when that is all it offers.
	 */
	auth_methods = member_strings(object,
	                              "token_endpoint_auth_methods_supported");

	if (auth_methods == NULL || strv_has(auth_methods, "none"))
		out->auth_method = g_strdup("none");
	else if (strv_has(auth_methods, "client_secret_post"))
		out->auth_method = g_strdup("client_secret_post");
	else
		out->auth_method = g_strdup("client_secret_basic");

	return g_steal_pointer(&out);
}

/* ── Registration ────────────────────────────────────────────────── */

gchar *
clawt_mcp_auth_registration_request(
	const ClawtMcpAuthMetadata *metadata,
	const gchar                *redirect_uri,
	const gchar                *client_name
){
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonGenerator) generator = json_generator_new();
	g_autoptr(JsonNode) root = NULL;

	g_return_val_if_fail(metadata != NULL, NULL);
	g_return_val_if_fail(redirect_uri != NULL, NULL);

	json_builder_begin_object(builder);

	json_builder_set_member_name(builder, "client_name");
	json_builder_add_string_value(builder, client_name != NULL
	                                       ? client_name : "clawtilla");

	json_builder_set_member_name(builder, "redirect_uris");
	json_builder_begin_array(builder);
	json_builder_add_string_value(builder, redirect_uri);
	json_builder_end_array(builder);

	json_builder_set_member_name(builder, "grant_types");
	json_builder_begin_array(builder);
	json_builder_add_string_value(builder, "authorization_code");
	json_builder_add_string_value(builder, "refresh_token");
	json_builder_end_array(builder);

	json_builder_set_member_name(builder, "response_types");
	json_builder_begin_array(builder);
	json_builder_add_string_value(builder, "code");
	json_builder_end_array(builder);

	json_builder_set_member_name(builder, "token_endpoint_auth_method");
	json_builder_add_string_value(builder, metadata->auth_method != NULL
	                                       ? metadata->auth_method : "none");

	if (metadata->scopes != NULL && *metadata->scopes != '\0') {
		json_builder_set_member_name(builder, "scope");
		json_builder_add_string_value(builder, metadata->scopes);
	}

	json_builder_end_object(builder);

	root = json_builder_get_root(builder);
	json_generator_set_root(generator, root);

	return json_generator_to_data(generator, NULL);
}

ClawtMcpAuthClient *
clawt_mcp_auth_parse_registration(
	const gchar  *json,
	gssize        length,
	GError      **error
){
	g_autoptr(JsonParser) parser = json_parser_new();
	ClawtMcpAuthClient *out;
	JsonObject *object;
	const gchar *refusal;
	const gchar *client_id;

	object = parse_object(parser, json, length, "registration response",
	                      error);

	if (object == NULL)
		return NULL;

	refusal = member_string(object, "error");

	if (refusal != NULL) {
		const gchar *description = member_string(object,
		                                         "error_description");

		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_AUTH,
		            "the authorization server refused to register "
		            "clawtilla: %s%s%s", refusal,
		            description != NULL ? " -- " : "",
		            description != NULL ? description : "");
		return NULL;
	}

	client_id = member_string(object, "client_id");

	if (client_id == NULL) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
		                    "the registration response has no client_id");
		return NULL;
	}

	out = g_new0(ClawtMcpAuthClient, 1);
	out->client_id = g_strdup(client_id);
	out->client_secret = g_strdup(member_string(object, "client_secret"));

	/* The server may grant a different method from the one asked for. */
	out->auth_method = g_strdup(member_string(object,
	                                          "token_endpoint_auth_method"));

	return out;
}

/* ── Deciding ────────────────────────────────────────────────────── */

ClawtMcpAuthPlan
clawt_mcp_auth_plan(
	gboolean                    flow,
	gboolean                    configured,
	gboolean                    remote,
	const ClawtMcpAuthMetadata *discovered,
	const gchar                *client_id
){
	if (!flow)
		return CLAWT_MCP_AUTH_PLAN_NONE;

	if (configured)
		return CLAWT_MCP_AUTH_PLAN_CONFIGURED;

	/* A flow with no endpoints and no URL has nothing to discover from. */
	if (!remote)
		return CLAWT_MCP_AUTH_PLAN_NONE;

	if (discovered == NULL)
		return CLAWT_MCP_AUTH_PLAN_DISCOVER;

	if (client_id != NULL && *client_id != '\0')
		return CLAWT_MCP_AUTH_PLAN_SIGN_IN;

	if (discovered->registration_endpoint != NULL)
		return CLAWT_MCP_AUTH_PLAN_REGISTER;

	return CLAWT_MCP_AUTH_PLAN_NO_CLIENT;
}

/* ── The 401 the agent hears ─────────────────────────────────────── */

static gchar *
render_object(
	JsonObject *object
){
	g_autoptr(JsonGenerator) generator = json_generator_new();
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);

	json_node_set_object(node, object);
	json_generator_set_root(generator, node);

	return json_generator_to_data(generator, NULL);
}

gchar *
clawt_mcp_auth_unauthorized_reply(
	const gchar *line,
	const gchar *connector,
	gboolean     sent_credential
){
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(JsonObject) reply = NULL;
	g_autofree gchar *text = NULL;
	JsonObject *request;
	const gchar *method;

	g_return_val_if_fail(connector != NULL, NULL);

	request = parse_object(parser, line, -1, "request", NULL);

	if (request == NULL || !json_object_has_member(request, "id"))
		return NULL;

	method = member_string(request, "method");

	/*
	 * Two different situations with two different fixes, so two
	 * sentences.  Both end the same way, because both are something only
	 * the person can do -- retrying spends a turn on a call that will be
	 * refused identically.
	 */
	if (sent_credential)
		text = g_strdup_printf(
			"The connector '%s' refused the credential clawtilla sent "
			"(HTTP 401), so nothing ran. Do not retry: the sign-in has "
			"expired or been withdrawn. Ask the person to sign in to '%s' "
			"again -- `clawtilla connector connect %s`, or Sign in on its "
			"row under Settings, Connectors.", connector, connector,
			connector);
	else
		text = g_strdup_printf(
			"The connector '%s' needs a sign-in before its tools work "
			"(HTTP 401), and nobody has signed in yet, so nothing ran. Do "
			"not retry. Ask the person to sign in to '%s' -- `clawtilla "
			"connector connect %s`, or Sign in on its row under Settings, "
			"Connectors.", connector, connector, connector);

	reply = json_object_new();
	json_object_set_string_member(reply, "jsonrpc", "2.0");
	json_object_set_member(reply, "id",
	                       json_node_copy(json_object_get_member(request,
	                                                             "id")));

	/*
	 * A failed tool *result* for a tool call, because that is what a
	 * model reads; a protocol error there is something most CLIs report
	 * as "the server failed" and nothing more.
	 */
	if (g_strcmp0(method, "tools/call") == 0) {
		JsonObject *result = json_object_new();
		JsonArray *content = json_array_new();
		JsonObject *item = json_object_new();

		json_object_set_string_member(item, "type", "text");
		json_object_set_string_member(item, "text", text);
		json_array_add_object_element(content, item);
		json_object_set_array_member(result, "content", content);
		json_object_set_boolean_member(result, "isError", TRUE);
		json_object_set_object_member(reply, "result", result);
	} else {
		JsonObject *error = json_object_new();

		json_object_set_int_member(error, "code", JSONRPC_UNAUTHORIZED);
		json_object_set_string_member(error, "message", text);
		json_object_set_object_member(reply, "error", error);
	}

	return render_object(reply);
}

/* ── The auth file ───────────────────────────────────────────────── */

gchar *
clawt_mcp_auth_path(
	const gchar *secrets_dir,
	const gchar *name
){
	g_autofree gchar *file = NULL;

	g_return_val_if_fail(secrets_dir != NULL, NULL);
	g_return_val_if_fail(name != NULL, NULL);

	/*
	 * A directory of its own rather than a suffix on the token file's
	 * name: `connector-x-auth.json` is also the token file of a
	 * connector called `x-auth`.  Folded as the token path folds, so a
	 * name from a config file cannot climb out of the directory.
	 */
	file = g_strdup_printf("%s.json", name);
	g_strdelimit(file, "/\\ \t", '_');

	return g_build_filename(secrets_dir, "connector-auth", file, NULL);
}

static void
add_member(
	JsonBuilder *builder,
	const gchar *name,
	const gchar *value
){
	if (value == NULL)
		return;

	json_builder_set_member_name(builder, name);
	json_builder_add_string_value(builder, value);
}

gboolean
clawt_mcp_auth_save(
	const gchar           *path,
	ClawtMcpAuthMetadata  *metadata,
	ClawtMcpAuthClient    *client,
	GError               **error
){
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonGenerator) generator = json_generator_new();
	g_autoptr(JsonNode) root = NULL;
	g_autofree gchar *dir = NULL;
	g_autofree gchar *text = NULL;
	gsize length = 0;
	gboolean ok;

	g_return_val_if_fail(path != NULL, FALSE);

	dir = g_path_get_dirname(path);

	if (!clawt_ensure_dir(dir, 0700, error))
		return FALSE;

	json_builder_begin_object(builder);

	if (metadata != NULL) {
		json_builder_set_member_name(builder, "metadata");
		json_builder_begin_object(builder);
		add_member(builder, "issuer", metadata->issuer);
		add_member(builder, "authorization_endpoint",
		           metadata->authorization_endpoint);
		add_member(builder, "token_endpoint", metadata->token_endpoint);
		add_member(builder, "registration_endpoint",
		           metadata->registration_endpoint);
		add_member(builder, "revocation_endpoint",
		           metadata->revocation_endpoint);
		add_member(builder, "scopes", metadata->scopes);
		add_member(builder, "resource", metadata->resource);
		add_member(builder, "source", metadata->source);
		add_member(builder, "auth_method", metadata->auth_method);
		json_builder_end_object(builder);
	}

	if (client != NULL) {
		json_builder_set_member_name(builder, "client");
		json_builder_begin_object(builder);
		add_member(builder, "client_id", client->client_id);
		add_member(builder, "client_secret", client->client_secret);
		add_member(builder, "auth_method", client->auth_method);
		json_builder_end_object(builder);
	}

	json_builder_end_object(builder);

	root = json_builder_get_root(builder);
	json_generator_set_root(generator, root);
	text = json_generator_to_data(generator, &length);

	ok = clawt_write_file_atomic(path, text, (gssize)length, 0600, FALSE,
	                             error);
	wipe(text);

	return ok;
}

gboolean
clawt_mcp_auth_load(
	const gchar            *path,
	ClawtMcpAuthMetadata  **out_metadata,
	ClawtMcpAuthClient    **out_client,
	GError                **error
){
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autofree gchar *text = NULL;
	g_autoptr(GError) read_error = NULL;
	JsonObject *object;

	g_return_val_if_fail(path != NULL, FALSE);

	if (out_metadata != NULL)
		*out_metadata = NULL;

	if (out_client != NULL)
		*out_client = NULL;

	if (!g_file_get_contents(path, &text, NULL, &read_error)) {
		if (g_error_matches(read_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
			return TRUE;

		g_propagate_error(error, g_steal_pointer(&read_error));
		return FALSE;
	}

	object = parse_object(parser, text, -1, "connector auth file", error);
	wipe(text);

	if (object == NULL)
		return FALSE;

	if (out_metadata != NULL && json_object_has_member(object, "metadata") &&
	    JSON_NODE_HOLDS_OBJECT(json_object_get_member(object, "metadata"))) {
		JsonObject *m = json_object_get_object_member(object, "metadata");
		ClawtMcpAuthMetadata *meta = g_new0(ClawtMcpAuthMetadata, 1);

		meta->issuer = g_strdup(member_string(m, "issuer"));
		meta->authorization_endpoint =
			g_strdup(member_string(m, "authorization_endpoint"));
		meta->token_endpoint = g_strdup(member_string(m, "token_endpoint"));
		meta->registration_endpoint =
			g_strdup(member_string(m, "registration_endpoint"));
		meta->revocation_endpoint =
			g_strdup(member_string(m, "revocation_endpoint"));
		meta->scopes = g_strdup(member_string(m, "scopes"));
		meta->resource = g_strdup(member_string(m, "resource"));
		meta->source = g_strdup(member_string(m, "source"));
		meta->auth_method = g_strdup(member_string(m, "auth_method"));

		/* Half a record is no record: a sign-in needs both endpoints. */
		if (meta->authorization_endpoint == NULL ||
		    meta->token_endpoint == NULL)
			clawt_mcp_auth_metadata_free(meta);
		else
			*out_metadata = meta;
	}

	if (out_client != NULL && json_object_has_member(object, "client") &&
	    JSON_NODE_HOLDS_OBJECT(json_object_get_member(object, "client"))) {
		JsonObject *c = json_object_get_object_member(object, "client");

		if (member_string(c, "client_id") != NULL) {
			ClawtMcpAuthClient *client = g_new0(ClawtMcpAuthClient, 1);

			client->client_id = g_strdup(member_string(c, "client_id"));
			client->client_secret =
				g_strdup(member_string(c, "client_secret"));
			client->auth_method =
				g_strdup(member_string(c, "auth_method"));
			*out_client = client;
		}
	}

	return TRUE;
}

/* ── The network legs ────────────────────────────────────────────── */

SoupMessage *
clawt_mcp_auth_document_message_new(
	const gchar *method,
	const gchar *url
){
	SoupMessage *message;

	g_return_val_if_fail(method != NULL, NULL);
	g_return_val_if_fail(url != NULL, NULL);

	/*
	 * Declared as carrying credentials, which a metadata GET does not:
	 * that is the declaration that refuses redirects, and refusing them
	 * is the point -- see the header.
	 */
	message = clawt_http_message_new(method, url,
	                                 CLAWT_HTTP_CARRIES_CREDENTIALS);

	if (message == NULL)
		return NULL;

	soup_message_headers_replace(soup_message_get_request_headers(message),
	                             "Accept", "application/json");

	return message;
}

static SoupSession *auth_session_override = NULL;

static SoupSession *
auth_session(
	void
){
	static SoupSession *session = NULL;

	if (auth_session_override != NULL)
		return auth_session_override;

	if (session == NULL)
		session = soup_session_new_with_options(
			"user-agent", USER_AGENT,
			"timeout", DISCOVERY_TIMEOUT_SECONDS, NULL);

	return session;
}

void
clawt_mcp_auth_set_session(
	SoupSession *session
){
	g_return_if_fail(session == NULL || SOUP_IS_SESSION(session));

	g_set_object(&auth_session_override, session);
}

/*
 * One bounded fetch: send, refuse a redirect, then read at most one byte
 * more than the cap -- which is how "too large" is told apart from
 * "exactly the cap" without reading the rest.
 */
typedef struct {
	SoupMessage  *message;
	GInputStream *stream;
	guint8       *buffer;
} Fetch;

static void
fetch_free(
	Fetch *fetch
){
	g_clear_object(&fetch->message);
	g_clear_object(&fetch->stream);
	g_free(fetch->buffer);
	g_free(fetch);
}

static void
on_fetch_read(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	g_autoptr(GTask) task = user_data;
	Fetch *fetch = g_task_get_task_data(task);
	g_autoptr(GError) error = NULL;
	gsize got = 0;

	if (!g_input_stream_read_all_finish(G_INPUT_STREAM(source), result, &got,
	                                    &error)) {
		g_task_return_error(task, g_steal_pointer(&error));
		return;
	}

	if (got > CLAWT_MCP_AUTH_MAX_DOCUMENT_BYTES) {
		g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
		                        "%s answered with more than %d bytes",
		                        g_uri_get_host(soup_message_get_uri(
		                            fetch->message)),
		                        CLAWT_MCP_AUTH_MAX_DOCUMENT_BYTES);
		return;
	}

	g_task_return_pointer(task, g_bytes_new(fetch->buffer, got),
	                      (GDestroyNotify)g_bytes_unref);
}

static void
on_fetch_sent(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	g_autoptr(GTask) task = user_data;
	Fetch *fetch = g_task_get_task_data(task);
	g_autoptr(GError) error = NULL;

	fetch->stream = soup_session_send_finish(SOUP_SESSION(source), result,
	                                         &error);

	if (fetch->stream == NULL) {
		g_task_return_error(task, g_steal_pointer(&error));
		return;
	}

	if (!clawt_http_check_redirect(fetch->message, &error)) {
		g_task_return_error(task, g_steal_pointer(&error));
		return;
	}

	fetch->buffer = g_malloc(CLAWT_MCP_AUTH_MAX_DOCUMENT_BYTES + 1);

	g_input_stream_read_all_async(fetch->stream, fetch->buffer,
	                              CLAWT_MCP_AUTH_MAX_DOCUMENT_BYTES + 1,
	                              G_PRIORITY_DEFAULT,
	                              g_task_get_cancellable(task),
	                              on_fetch_read, g_object_ref(task));
}

/* Sends @message; the task's result is the body, the status is on @message. */
static void
fetch_async(
	SoupMessage         *message,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	GTask *task = g_task_new(NULL, cancellable, callback, user_data);
	Fetch *fetch = g_new0(Fetch, 1);

	fetch->message = g_object_ref(message);
	g_task_set_task_data(task, fetch, (GDestroyNotify)fetch_free);

	soup_session_send_async(auth_session(), message, G_PRIORITY_DEFAULT,
	                        cancellable, on_fetch_sent, task);
}

static GBytes *
fetch_finish(
	GAsyncResult  *result,
	GError       **error
){
	return g_task_propagate_pointer(G_TASK(result), error);
}

/* ── Discovery ───────────────────────────────────────────────────── */

typedef struct {
	gchar       *mcp_url;
	GStrv        candidates;
	guint        next;
	gboolean     resource_stage;
	gchar       *issuer;
	gchar       *scopes;
	gboolean     named;
	SoupMessage *message;
	GError      *last_error;
} Discovery;

static void
discovery_free(
	Discovery *discovery
){
	g_free(discovery->mcp_url);
	g_strfreev(discovery->candidates);
	g_free(discovery->issuer);
	g_free(discovery->scopes);
	g_clear_object(&discovery->message);
	g_clear_error(&discovery->last_error);
	g_free(discovery);
}

static void discovery_next(GTask *task);

/* Remembers why a candidate failed, so the final error can say. */
static void
discovery_note(
	Discovery *discovery,
	GError    *error
){
	g_clear_error(&discovery->last_error);
	discovery->last_error = error;
}

/* Moves from the protected-resource documents to the server's metadata. */
static void
discovery_begin_server_stage(
	GTask     *task,
	Discovery *discovery
){
	if (discovery->issuer == NULL) {
		g_autoptr(GUri) uri = parse_web_url(discovery->mcp_url);

		/*
		 * No document named a server, so the MCP server's own origin is
		 * assumed to be its own authorization server -- what clients of
		 * the 2025-03-26 spec did, and what servers of that vintage
		 * still expect.
		 */
		discovery->issuer = (uri != NULL) ? uri_origin(uri) : NULL;
		discovery->named = FALSE;
	}

	if (discovery->issuer == NULL ||
	    !clawt_mcp_auth_url_allowed(discovery->issuer, discovery->mcp_url)) {
		g_task_return_new_error(task, CLAWT_ERROR,
		                        CLAWT_ERROR_PERMISSION_DENIED,
		                        "%s names an authorization server clawtilla "
		                        "will not use: %s", discovery->mcp_url,
		                        discovery->issuer != NULL
		                        ? discovery->issuer : "(none)");
		g_object_unref(task);
		return;
	}

	g_strfreev(discovery->candidates);
	discovery->candidates = clawt_mcp_auth_server_candidates(discovery->issuer);
	discovery->next = 0;
	discovery->resource_stage = FALSE;

	discovery_next(task);
}

static void
on_discovery_fetched(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	GTask *task = user_data;
	Discovery *discovery = g_task_get_task_data(task);
	g_autoptr(GBytes) body = NULL;
	GError *error = NULL;
	const gchar *data;
	gsize length = 0;
	guint status;

	body = fetch_finish(result, &error);

	if (body == NULL) {
		discovery_note(discovery, error);
		discovery_next(task);
		return;
	}

	status = soup_message_get_status(discovery->message);

	if (status != SOUP_STATUS_OK) {
		discovery_note(discovery,
		               g_error_new(CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
		                           "%s answered %u",
		                           g_uri_get_host(soup_message_get_uri(
		                               discovery->message)), status));
		discovery_next(task);
		return;
	}

	data = g_bytes_get_data(body, &length);

	if (discovery->resource_stage) {
		g_autofree gchar *issuer = NULL;
		g_autofree gchar *scopes = NULL;

		if (!clawt_mcp_auth_parse_resource(data, (gssize)length,
		                                   discovery->mcp_url, &issuer,
		                                   &scopes, &error)) {
			discovery_note(discovery, error);
			discovery_next(task);
			return;
		}

		discovery->issuer = g_steal_pointer(&issuer);
		discovery->scopes = g_steal_pointer(&scopes);
		discovery->named = (discovery->issuer != NULL);

		discovery_begin_server_stage(task, discovery);
		return;
	}

	{
		ClawtMcpAuthMetadata *metadata = clawt_mcp_auth_parse_server(
			data, (gssize)length, discovery->issuer, discovery->mcp_url,
			discovery->scopes, discovery->named, &error);

		if (metadata == NULL) {
			discovery_note(discovery, error);
			discovery_next(task);
			return;
		}

		g_task_return_pointer(task, metadata,
		                      (GDestroyNotify)clawt_mcp_auth_metadata_free);
		g_object_unref(task);
	}
}

/* Tries the next candidate URL of whichever stage is running. */
static void
discovery_next(
	GTask *task
){
	Discovery *discovery = g_task_get_task_data(task);

	while (discovery->candidates != NULL &&
	       discovery->candidates[discovery->next] != NULL) {
		const gchar *url = discovery->candidates[discovery->next++];

		if (!clawt_mcp_auth_url_allowed(url, discovery->mcp_url)) {
			discovery_note(discovery,
			               g_error_new(CLAWT_ERROR,
			                           CLAWT_ERROR_PERMISSION_DENIED,
			                           "refused to fetch %s: not https, or "
			                           "loopback named by a server that is "
			                           "not", url));
			continue;
		}

		g_clear_object(&discovery->message);
		discovery->message = clawt_mcp_auth_document_message_new("GET", url);

		if (discovery->message == NULL)
			continue;

		fetch_async(discovery->message, g_task_get_cancellable(task),
		            on_discovery_fetched, task);
		return;
	}

	/* Out of candidates. */
	if (discovery->resource_stage) {
		discovery_begin_server_stage(task, discovery);
		return;
	}

	g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
	                        "found no authorization server for %s (%s)",
	                        discovery->mcp_url,
	                        discovery->last_error != NULL
	                        ? discovery->last_error->message
	                        : "no metadata at any well-known address");
	g_object_unref(task);
}

void
clawt_mcp_auth_discover_async(
	const gchar         *mcp_url,
	const gchar         *hint,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	GTask *task;
	Discovery *discovery;

	g_return_if_fail(mcp_url != NULL);

	task = g_task_new(NULL, cancellable, callback, user_data);
	discovery = g_new0(Discovery, 1);
	discovery->mcp_url = g_strdup(mcp_url);
	discovery->resource_stage = TRUE;
	discovery->candidates = clawt_mcp_auth_resource_candidates(mcp_url, hint);
	g_task_set_task_data(task, discovery, (GDestroyNotify)discovery_free);

	/* The configured URL itself must pass the rule it applies to others. */
	if (!clawt_mcp_auth_url_allowed(mcp_url, mcp_url)) {
		g_task_return_new_error(task, CLAWT_ERROR,
		                        CLAWT_ERROR_PERMISSION_DENIED,
		                        "%s is not https; only a loopback test "
		                        "server may be reached over plain http",
		                        mcp_url);
		g_object_unref(task);
		return;
	}

	discovery_next(task);
}

ClawtMcpAuthMetadata *
clawt_mcp_auth_discover_finish(
	GAsyncResult  *result,
	GError       **error
){
	g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);

	return g_task_propagate_pointer(G_TASK(result), error);
}

/* ── Registering ─────────────────────────────────────────────────── */

static void
on_registered(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	g_autoptr(GTask) task = user_data;
	SoupMessage *message = g_task_get_task_data(task);
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;
	ClawtMcpAuthClient *client;
	const gchar *data;
	gsize length = 0;
	guint status;

	body = fetch_finish(result, &error);

	if (body == NULL) {
		g_task_return_error(task, g_steal_pointer(&error));
		return;
	}

	status = soup_message_get_status(message);
	data = g_bytes_get_data(body, &length);
	client = clawt_mcp_auth_parse_registration(data, (gssize)length, &error);

	/*
	 * Read before the status is judged, because a refusal arrives as a
	 * 400 with an `error` member that says why -- "invalid_redirect_uri"
	 * is something a person can act on and "answered 400" is not.
	 */
	if (client == NULL) {
		g_task_return_error(task, g_steal_pointer(&error));
		return;
	}

	if (status != SOUP_STATUS_OK && status != SOUP_STATUS_CREATED) {
		clawt_mcp_auth_client_free(client);
		g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_AUTH,
		                        "registration answered %u", status);
		return;
	}

	g_task_return_pointer(task, client,
	                      (GDestroyNotify)clawt_mcp_auth_client_free);
}

void
clawt_mcp_auth_register_async(
	const ClawtMcpAuthMetadata *metadata,
	const gchar                *redirect_uri,
	const gchar                *client_name,
	GCancellable               *cancellable,
	GAsyncReadyCallback         callback,
	gpointer                    user_data
){
	GTask *task;
	g_autoptr(SoupMessage) message = NULL;
	g_autoptr(GBytes) bytes = NULL;
	gchar *body;

	g_return_if_fail(metadata != NULL);
	g_return_if_fail(redirect_uri != NULL);

	task = g_task_new(NULL, cancellable, callback, user_data);

	if (metadata->registration_endpoint == NULL) {
		g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
		                        "%s offers no dynamic client registration; "
		                        "register a client there and set client_id",
		                        metadata->issuer);
		g_object_unref(task);
		return;
	}

	message = clawt_mcp_auth_document_message_new(
		"POST", metadata->registration_endpoint);

	if (message == NULL) {
		g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
		                        "'%s' is not a URL that can be dialled",
		                        metadata->registration_endpoint);
		g_object_unref(task);
		return;
	}

	body = clawt_mcp_auth_registration_request(metadata, redirect_uri,
	                                           client_name);
	bytes = g_bytes_new_take(body, strlen(body));
	soup_message_set_request_body_from_bytes(message, "application/json",
	                                         bytes);

	g_task_set_task_data(task, g_object_ref(message), g_object_unref);

	fetch_async(message, cancellable, on_registered, task);
}

ClawtMcpAuthClient *
clawt_mcp_auth_register_finish(
	GAsyncResult  *result,
	GError       **error
){
	g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);

	return g_task_propagate_pointer(G_TASK(result), error);
}
