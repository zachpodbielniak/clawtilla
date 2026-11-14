/*
 * clawt-mcp-auth.h - Finding out how to sign in to a remote MCP server
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A remote MCP server that wants a sign-in says so with a 401, and the
 * MCP authorization spec says where to go next: the server's RFC 9728
 * protected-resource document names an authorization server, that
 * server's RFC 8414 metadata names its endpoints, and RFC 7591 dynamic
 * client registration gives clawtilla a client id when nobody
 * registered one by hand.  Before this, a remote connector worked only
 * when somebody had copied an `auth_url` and a `token_url` into a
 * catalogue file -- which, for a server that publishes all of it, is a
 * person doing by hand what the server is offering to do for them.
 *
 * Everything that reads a document, decides what to do next or builds a
 * request is a pure function here, so the whole chain is tested as data
 * with no socket.  The three network legs (the metadata fetches and the
 * registration) are the only things that leave the machine, and they
 * run only when a client asks for a sign-in.
 *
 * Two rules hold throughout, and both came from the same place: a URL in
 * a metadata document is the remote server's input, not ours.
 *
 * - **Nothing here follows a redirect.**  A token endpoint is where a
 *   sign-in code is spent, and a registration response carries a client
 *   secret; libsoup would carry either to whatever host a 3xx named.
 * - **Nothing here dials plain HTTP off this machine**, and a document
 *   from a public server may not aim a fetch at loopback.  Without the
 *   second half, a hostile server's `resource_metadata` pointing at
 *   `http://127.0.0.1:...` would turn the daemon into a client of this
 *   machine's own unauthenticated loopback services.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <gio/gio.h>
#include <glib-object.h>
#include <libsoup/soup.h>

G_BEGIN_DECLS

/**
 * CLAWT_MCP_AUTH_MAX_DOCUMENT_BYTES:
 *
 * The most of a metadata or registration document that is read.
 *
 * These come from a server nobody here controls, so the limit is on what
 * is read rather than on what arrived: a check after the read is a check
 * on memory already spent.
 */
#define CLAWT_MCP_AUTH_MAX_DOCUMENT_BYTES (65536)

/**
 * ClawtMcpAuthMetadata:
 * @issuer: the authorization server, as its own metadata names it
 * @authorization_endpoint: where a person approves the request
 * @token_endpoint: where a code is exchanged and a token renewed
 * @registration_endpoint: (nullable): RFC 7591 dynamic client
 *   registration, when the server offers it
 * @revocation_endpoint: (nullable): RFC 7009 revocation, when offered
 * @scopes: (nullable): the protected resource's `scopes_supported`,
 *   space separated
 * @resource: the MCP server's URL, sent as the RFC 8707 `resource` so
 *   every token is bound to that one server
 * @source: how the authorization server was found:
 *   `"protected-resource"` when an RFC 9728 document named it, or
 *   `"origin"` when none did and the MCP server's own origin was assumed
 *   to be the issuer, as MCP 2025-03-26 clients do
 * @auth_method: the `token_endpoint_auth_method` a registration should
 *   ask for: `"none"` when the server takes public clients, else
 *   `"client_secret_post"`
 *
 * Everything discovery learned about signing in to one remote MCP
 * server.  None of it is secret; it is kept in the connector's 0600
 * auth file beside the registered client because it belongs to it.
 */
typedef struct {
	gchar *issuer;
	gchar *authorization_endpoint;
	gchar *token_endpoint;
	gchar *registration_endpoint;
	gchar *revocation_endpoint;
	gchar *scopes;
	gchar *resource;
	gchar *source;
	gchar *auth_method;
} ClawtMcpAuthMetadata;

#define CLAWT_TYPE_MCP_AUTH_METADATA (clawt_mcp_auth_metadata_get_type())

GType
clawt_mcp_auth_metadata_get_type(
	void
) G_GNUC_CONST;

/**
 * clawt_mcp_auth_metadata_copy:
 * @self: metadata
 *
 * Returns: (transfer full): a deep copy
 */
ClawtMcpAuthMetadata *
clawt_mcp_auth_metadata_copy(
	ClawtMcpAuthMetadata *self
);

/**
 * clawt_mcp_auth_metadata_free:
 * @self: (transfer full) (nullable): metadata
 *
 * Frees @self.
 */
void
clawt_mcp_auth_metadata_free(
	ClawtMcpAuthMetadata *self
);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtMcpAuthMetadata, clawt_mcp_auth_metadata_free)

/**
 * ClawtMcpAuthClient:
 * @client_id: the client id the authorization server issued
 * @client_secret: (nullable): a secret, when the server issued one
 *
 * A client registered by RFC 7591 dynamic client registration.
 *
 * Kept only in the connector's 0600 auth file.  The id crosses nothing;
 * the secret is a credential like any other and is wiped when freed.
 */
typedef struct {
	gchar *client_id;
	gchar *client_secret;
} ClawtMcpAuthClient;

#define CLAWT_TYPE_MCP_AUTH_CLIENT (clawt_mcp_auth_client_get_type())

GType
clawt_mcp_auth_client_get_type(
	void
) G_GNUC_CONST;

/**
 * clawt_mcp_auth_client_copy:
 * @self: a registered client
 *
 * Returns: (transfer full): a deep copy
 */
ClawtMcpAuthClient *
clawt_mcp_auth_client_copy(
	ClawtMcpAuthClient *self
);

/**
 * clawt_mcp_auth_client_free:
 * @self: (transfer full) (nullable): a registered client
 *
 * Frees @self, wiping the secret first.
 */
void
clawt_mcp_auth_client_free(
	ClawtMcpAuthClient *self
);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtMcpAuthClient, clawt_mcp_auth_client_free)

/**
 * clawt_mcp_auth_parse_challenge:
 * @www_authenticate: (nullable): a 401's `WWW-Authenticate` header
 *
 * The RFC 9728 `resource_metadata` URL a 401 points at, if it names one.
 *
 * Read as data, quoted or bare, in any challenge the header carries.
 * Whether the URL may be fetched is clawt_mcp_auth_url_allowed()'s
 * question, not this one's.
 *
 * Returns: (transfer full) (nullable): the URL, or %NULL
 */
gchar *
clawt_mcp_auth_parse_challenge(
	const gchar *www_authenticate
);

/**
 * clawt_mcp_auth_url_allowed:
 * @url: a URL discovery is about to fetch or hand to a browser
 * @trusted_url: the MCP server URL the operator configured
 *
 * Whether @url may be used.
 *
 * HTTPS always may; plain HTTP only on loopback, and only when
 * @trusted_url is itself on loopback -- a local test server naming a
 * sign-in server on another local port is code the operator started,
 * and a public server naming one is an attack on this machine.  A URL
 * carrying a user name or password is refused outright.
 *
 * Returns: %TRUE if @url may be used
 */
gboolean
clawt_mcp_auth_url_allowed(
	const gchar *url,
	const gchar *trusted_url
);

/**
 * clawt_mcp_auth_resource_candidates:
 * @mcp_url: the MCP server's URL
 * @hint: (nullable): the `resource_metadata` a 401 named
 *
 * Where to look for the RFC 9728 protected-resource document, in order:
 * the hint, the well-known address for this URL's path, then the
 * well-known address at the origin.
 *
 * Returns: (transfer full) (array zero-terminated=1): the URLs
 */
GStrv
clawt_mcp_auth_resource_candidates(
	const gchar *mcp_url,
	const gchar *hint
);

/**
 * clawt_mcp_auth_server_candidates:
 * @issuer: the authorization server's issuer identifier
 *
 * Where to look for its RFC 8414 metadata, in order: the OAuth
 * well-known address, then OpenID Connect's (path-inserted, then
 * path-appended for an issuer with a path).
 *
 * Returns: (transfer full) (array zero-terminated=1): the URLs
 */
GStrv
clawt_mcp_auth_server_candidates(
	const gchar *issuer
);

/**
 * clawt_mcp_auth_parse_resource:
 * @json: a protected-resource document
 * @length: its length, or -1 if NUL-terminated
 * @mcp_url: the MCP server's URL
 * @out_issuer: (out) (transfer full) (optional) (nullable): the first
 *   authorization server it names, or %NULL when it names none
 * @out_scopes: (out) (transfer full) (optional) (nullable): its
 *   `scopes_supported`, space separated
 * @error: (out) (optional): return location for a #GError
 *
 * Reads an RFC 9728 protected-resource document.
 *
 * Its `resource` must be the server it was fetched for (RFC 9728
 * section 3.3): a document describing some other resource is one a
 * hostile server copied from somebody else's, and trusting it would
 * send a person to sign in at a server that is not this one's.
 *
 * Returns: %TRUE if the document was read and describes @mcp_url
 */
gboolean
clawt_mcp_auth_parse_resource(
	const gchar  *json,
	gssize        length,
	const gchar  *mcp_url,
	gchar       **out_issuer,
	gchar       **out_scopes,
	GError      **error
);

/**
 * clawt_mcp_auth_parse_server:
 * @json: an authorization server's metadata
 * @length: its length, or -1 if NUL-terminated
 * @issuer: the issuer it was fetched for
 * @mcp_url: the MCP server's URL, which decides what may be dialled
 * @scopes: (nullable): the scopes the protected resource advertised
 * @named: whether a protected-resource document named @issuer, as
 *   opposed to the MCP server's origin being assumed
 * @error: (out) (optional): return location for a #GError
 *
 * Reads RFC 8414 authorization-server metadata into what a sign-in
 * needs.
 *
 * Refused when its `issuer` is not @issuer (RFC 8414 section 3.3), when
 * an endpoint is not one clawt_mcp_auth_url_allowed() would dial, or
 * when the server does not offer PKCE with S256 -- a flow without it is
 * one a code intercepted on the way back could complete.
 *
 * Returns: (transfer full) (nullable): the metadata, or %NULL on error
 */
ClawtMcpAuthMetadata *
clawt_mcp_auth_parse_server(
	const gchar  *json,
	gssize        length,
	const gchar  *issuer,
	const gchar  *mcp_url,
	const gchar  *scopes,
	gboolean      named,
	GError      **error
);

/**
 * clawt_mcp_auth_registration_request:
 * @metadata: what discovery found
 * @redirect_uri: the loopback redirect the flow will use
 * @client_name: what the registration calls this client
 *
 * The RFC 7591 registration request, as a JSON body.
 *
 * Asks for the authorization-code grant with refresh tokens and the one
 * redirect URI clawtilla listens on, as a public client when the server
 * takes one -- a secret held by a daemon on a laptop protects nothing a
 * PKCE verifier does not.
 *
 * Returns: (transfer full): the JSON body
 */
gchar *
clawt_mcp_auth_registration_request(
	const ClawtMcpAuthMetadata *metadata,
	const gchar                *redirect_uri,
	const gchar                *client_name
);

/**
 * clawt_mcp_auth_parse_registration:
 * @json: a registration response
 * @length: its length, or -1 if NUL-terminated
 * @error: (out) (optional): return location for a #GError
 *
 * Reads an RFC 7591 registration response, or the server's refusal of
 * one (`error` and `error_description`) as a #GError naming it.
 *
 * Returns: (transfer full) (nullable): the client, or %NULL on error
 */
ClawtMcpAuthClient *
clawt_mcp_auth_parse_registration(
	const gchar  *json,
	gssize        length,
	GError      **error
);

/**
 * ClawtMcpAuthPlan:
 * @CLAWT_MCP_AUTH_PLAN_NONE: nothing to sign in to: not a remote
 *   connector, or one whose provider takes a key rather than a flow
 * @CLAWT_MCP_AUTH_PLAN_CONFIGURED: the catalogue names the endpoints by
 *   hand, and they win over anything a server publishes
 * @CLAWT_MCP_AUTH_PLAN_DISCOVER: a remote server whose authorization
 *   server is not known yet; discovery comes first
 * @CLAWT_MCP_AUTH_PLAN_REGISTER: discovered, with no client id to sign
 *   in as; register one
 * @CLAWT_MCP_AUTH_PLAN_SIGN_IN: discovered, with a client id configured
 *   or registered; sign in
 * @CLAWT_MCP_AUTH_PLAN_NO_CLIENT: discovered, with no client id and no
 *   registration endpoint -- somebody has to register one by hand
 *
 * What a sign-in on one connector has to do next.
 */
typedef enum {
	CLAWT_MCP_AUTH_PLAN_NONE = 0,
	CLAWT_MCP_AUTH_PLAN_CONFIGURED,
	CLAWT_MCP_AUTH_PLAN_DISCOVER,
	CLAWT_MCP_AUTH_PLAN_REGISTER,
	CLAWT_MCP_AUTH_PLAN_SIGN_IN,
	CLAWT_MCP_AUTH_PLAN_NO_CLIENT
} ClawtMcpAuthPlan;

/**
 * clawt_mcp_auth_plan:
 * @flow: whether the provider obtains its credential by a flow at all
 *   (device or PKCE) rather than a pasted key
 * @configured: whether the catalogue entry names both its authorization
 *   and token endpoints
 * @remote: whether the connector reaches its server by URL
 * @discovered: (nullable): what discovery found, if it has run
 * @client_id: (nullable): a client id from the configuration or from an
 *   earlier registration
 *
 * Decides the next step of a sign-in.
 *
 * Hand-configured endpoints win: a catalogue entry that names them is a
 * decision somebody made, and a server's metadata is only its claim.  A
 * configured client id wins over registering one, for the same reason.
 *
 * Returns: the next step
 */
ClawtMcpAuthPlan
clawt_mcp_auth_plan(
	gboolean                    flow,
	gboolean                    configured,
	gboolean                    remote,
	const ClawtMcpAuthMetadata *discovered,
	const gchar                *client_id
);

/**
 * clawt_mcp_auth_unauthorized_reply:
 * @line: the JSON-RPC request the server answered 401
 * @connector: the connector's name
 * @sent_credential: whether the request carried a credential
 *
 * What the agent hears instead of a bare 401.
 *
 * Passed through, a 401 tells an agent's CLI to start a sign-in of its
 * own, which cannot work -- the relay holds the credential, and the
 * agent's own Authorization never reaches the server -- so the CLI
 * reports only that the server failed.  A server that lists its tools to
 * anybody and refuses only `tools/call` makes it worse: the tools look
 * fine until the first one is used.  So the refusal is answered here, in
 * words naming the connector and the sign-in that fixes it.
 *
 * A `tools/call` gets a tool result with `isError`, because that is the
 * shape a model reads; any other request gets a JSON-RPC error carrying
 * the same sentence.  A notification has nothing to answer into.
 *
 * Returns: (transfer full) (nullable): the reply, or %NULL when @line
 *   has no id
 */
gchar *
clawt_mcp_auth_unauthorized_reply(
	const gchar *line,
	const gchar *connector,
	gboolean     sent_credential
);

/**
 * clawt_mcp_auth_path:
 * @secrets_dir: where the daemon keeps credentials
 * @name: the connector's name
 *
 * Where one connector's discovered metadata and registered client live,
 * beside its token file and under the same name folding.
 *
 * Returns: (transfer full): the path
 */
gchar *
clawt_mcp_auth_path(
	const gchar *secrets_dir,
	const gchar *name
);

/**
 * clawt_mcp_auth_save:
 * @path: where to write
 * @metadata: (nullable): what discovery found
 * @client: (nullable): a registered client
 * @error: (out) (optional): return location for a #GError
 *
 * Writes the auth file, 0600 from the first byte.
 *
 * Returns: %TRUE on success
 */
gboolean
clawt_mcp_auth_save(
	const gchar           *path,
	ClawtMcpAuthMetadata  *metadata,
	ClawtMcpAuthClient    *client,
	GError               **error
);

/**
 * clawt_mcp_auth_load:
 * @path: the auth file
 * @out_metadata: (out) (transfer full) (optional) (nullable): what
 *   discovery found, if it has run
 * @out_client: (out) (transfer full) (optional) (nullable): the
 *   registered client, if there is one
 * @error: (out) (optional): return location for a #GError
 *
 * Reads the auth file.  A file that does not exist is not an error: it
 * is a connector nothing has been discovered for yet.
 *
 * Returns: %TRUE unless the file exists and could not be read
 */
gboolean
clawt_mcp_auth_load(
	const gchar            *path,
	ClawtMcpAuthMetadata  **out_metadata,
	ClawtMcpAuthClient    **out_client,
	GError                **error
);

/**
 * clawt_mcp_auth_document_message_new:
 * @method: `GET` or `POST`
 * @url: where to send it
 *
 * The request every discovery leg is sent with: redirects refused.
 *
 * A metadata fetch carries no credential, and refuses redirects anyway:
 * the registration and token endpoints it names are where a client
 * secret and a sign-in code go, so a server that moved has to say where
 * in its own metadata, not in a 3xx.
 *
 * Returns: (transfer full) (nullable): the message, or %NULL when @url
 *   cannot be parsed
 */
SoupMessage *
clawt_mcp_auth_document_message_new(
	const gchar *method,
	const gchar *url
);

/**
 * clawt_mcp_auth_discover_async:
 * @mcp_url: the MCP server's URL
 * @hint: (nullable): the `resource_metadata` a 401 named
 * @cancellable: (nullable): a #GCancellable
 * @callback: called with the result
 * @user_data: for @callback
 *
 * Fetches the protected-resource document and then the authorization
 * server's metadata.
 *
 * Every candidate URL is checked with clawt_mcp_auth_url_allowed()
 * before anything is sent to it, and every document is read up to
 * %CLAWT_MCP_AUTH_MAX_DOCUMENT_BYTES and no further.
 */
void
clawt_mcp_auth_discover_async(
	const gchar         *mcp_url,
	const gchar         *hint,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
);

/**
 * clawt_mcp_auth_discover_finish:
 * @result: the #GAsyncResult
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): what discovery found, or %NULL
 *   with @error naming the step that failed
 */
ClawtMcpAuthMetadata *
clawt_mcp_auth_discover_finish(
	GAsyncResult  *result,
	GError       **error
);

/**
 * clawt_mcp_auth_register_async:
 * @metadata: what discovery found; must carry a registration endpoint
 * @redirect_uri: the loopback redirect the flow will use
 * @client_name: what to call this client
 * @cancellable: (nullable): a #GCancellable
 * @callback: called with the result
 * @user_data: for @callback
 *
 * Registers a client by RFC 7591 dynamic client registration.
 */
void
clawt_mcp_auth_register_async(
	const ClawtMcpAuthMetadata *metadata,
	const gchar                *redirect_uri,
	const gchar                *client_name,
	GCancellable               *cancellable,
	GAsyncReadyCallback         callback,
	gpointer                    user_data
);

/**
 * clawt_mcp_auth_register_finish:
 * @result: the #GAsyncResult
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the registered client, or %NULL
 */
ClawtMcpAuthClient *
clawt_mcp_auth_register_finish(
	GAsyncResult  *result,
	GError       **error
);

G_END_DECLS
