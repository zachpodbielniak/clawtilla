/*
 * clawt-oauth.h - Obtaining a credential without the agent ever holding it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Two flows, and the choice between them is not a matter of taste.
 *
 * The device authorization grant (RFC 8628) is the one that suits a
 * daemon.  It needs no redirect URI, no listening socket and no browser
 * on the same machine: clawtilla asks for a code, the person types that
 * code into a page on whatever device they happen to be holding, and
 * clawtilla polls until they have.  A workstation reached over SSH can
 * be connected from a phone, which is the case that matters here.
 *
 * The authorization code grant with PKCE (RFC 7636) exists because a
 * great many services never implemented device grant.  It costs a
 * loopback listener and a redirect URI registered with the provider in
 * advance, so it is the second choice rather than the default.
 *
 * Everything that parses a wire format is a plain function taking text
 * and returning a struct, separated from everything that touches the
 * network.  A token response arrives once, from a server nobody controls,
 * on the one path a person is watching -- and a mistake in reading it is
 * otherwise only reproducible by starting the whole flow again.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <gio/gio.h>
#include <glib-object.h>

G_BEGIN_DECLS

/**
 * ClawtOauthToken:
 * @access_token: the credential itself
 * @refresh_token: (nullable): what renews it, when the provider issues one
 * @token_type: (nullable): usually "Bearer"
 * @scopes: (nullable): what was actually granted, which may be less than asked
 * @expires_at: when @access_token stops working, or 0 if it does not
 *
 * A credential as a provider handed it over.
 *
 * @expires_at is an absolute time, computed when the response arrives,
 * and never the `expires_in` the wire carries.  A duration is only
 * meaningful next to the moment it was received: stored as it came, an
 * hour-long token reads as valid for an hour every time the daemon
 * restarts, so a token that expired overnight looks fresh each morning
 * and every call with it fails.
 *
 * @scopes is what was granted rather than what was requested, because a
 * person can untick things on the consent screen.  An agent told it has
 * write access that was refused spends its turns discovering that one
 * call at a time.
 */
typedef struct {
    gchar  *access_token;
    gchar  *refresh_token;
    gchar  *token_type;
    gchar  *scopes;
    gint64  expires_at;
} ClawtOauthToken;

#define CLAWT_TYPE_OAUTH_TOKEN (clawt_oauth_token_get_type())

GType clawt_oauth_token_get_type(void) G_GNUC_CONST;

ClawtOauthToken *clawt_oauth_token_copy(ClawtOauthToken *self);

/**
 * clawt_oauth_token_free:
 * @self: (transfer full) (nullable): a token
 *
 * Frees a token, wiping the secret fields before releasing them.
 *
 * Wiping is not theatre here.  The daemon is long-lived and holds tokens
 * for a whole fleet; freed heap is handed straight back out, and a core
 * dump or a swapped page then carries live credentials for services the
 * person may not even remember connecting.
 */
void clawt_oauth_token_free(ClawtOauthToken *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtOauthToken, clawt_oauth_token_free)

/**
 * clawt_oauth_token_parse:
 * @json: a token endpoint's response body
 * @length: its length, or -1 if NUL-terminated
 * @now: the current time in seconds, for computing @expires_at
 * @error: (out) (optional): return location for a #GError
 *
 * Reads a token response.
 *
 * @now is a parameter rather than a call to g_get_real_time() so that
 * expiry arithmetic can be asserted on at a fixed instant instead of
 * against whatever the clock says while the test runs.
 *
 * Returns: (transfer full) (nullable): the token, or %NULL on error
 */
ClawtOauthToken *clawt_oauth_token_parse(const gchar  *json,
                                         gssize        length,
                                         gint64        now,
                                         GError      **error);

/**
 * clawt_oauth_token_is_expired:
 * @self: a token
 * @now: the current time in seconds
 * @skew: seconds of headroom to insist on
 *
 * Whether @self should be renewed before being used.
 *
 * @skew exists because a token that expires during the request carrying
 * it fails exactly like a wrong one, and the agent sees only the failure.
 *
 * Returns: %TRUE if it has expired or is about to
 */
gboolean clawt_oauth_token_is_expired(ClawtOauthToken *self,
                                      gint64           now,
                                      gint64           skew);

/**
 * clawt_oauth_token_save:
 * @self: a token
 * @path: where to write it
 * @error: (out) (optional): return location for a #GError
 *
 * Writes @self to @path as JSON, mode 0600.
 *
 * Returns: %TRUE on success
 */
gboolean clawt_oauth_token_save(ClawtOauthToken  *self,
                                const gchar      *path,
                                GError          **error);

/**
 * clawt_oauth_token_load:
 * @path: a file written by clawt_oauth_token_save()
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the token, or %NULL
 */
ClawtOauthToken *clawt_oauth_token_load(const gchar *path, GError **error);

/**
 * ClawtDeviceCode:
 * @device_code: what clawtilla polls with; not for showing anybody
 * @user_code: what the person types in
 * @verification_uri: where they type it
 * @verification_uri_complete: (nullable): the same page with the code in it
 * @interval: seconds the provider asks us to wait between polls
 * @expires_at: when the whole attempt stops being valid
 *
 * The provider's answer to "somebody here would like to connect".
 *
 * @device_code and @user_code are easy to confuse and must not be: one
 * is a secret that authorises the exchange, the other is a short string
 * meant to be read aloud.  Showing the device code would put a live
 * credential on the screen and in the scrollback.
 */
typedef struct {
    gchar  *device_code;
    gchar  *user_code;
    gchar  *verification_uri;
    gchar  *verification_uri_complete;
    gint    interval;
    gint64  expires_at;
} ClawtDeviceCode;

#define CLAWT_TYPE_DEVICE_CODE (clawt_device_code_get_type())

GType clawt_device_code_get_type(void) G_GNUC_CONST;

ClawtDeviceCode *clawt_device_code_copy(ClawtDeviceCode *self);

/**
 * clawt_device_code_free:
 * @self: (transfer full) (nullable): a set of device codes
 *
 * Frees them, wiping @device_code first -- it authorises the exchange
 * and is a live credential for as long as the flow is open.
 */
void clawt_device_code_free(ClawtDeviceCode *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtDeviceCode, clawt_device_code_free)

/**
 * clawt_oauth_parse_device_code:
 * @json: the device endpoint's response body
 * @length: its length, or -1 if NUL-terminated
 * @now: the current time in seconds
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the codes, or %NULL on error
 */
ClawtDeviceCode *clawt_oauth_parse_device_code(const gchar  *json,
                                               gssize        length,
                                               gint64        now,
                                               GError      **error);

/**
 * ClawtOauthPollResult:
 * @CLAWT_OAUTH_POLL_GRANTED: the token is in hand
 * @CLAWT_OAUTH_POLL_PENDING: nobody has typed the code in yet
 * @CLAWT_OAUTH_POLL_SLOW_DOWN: polling too fast; wait longer from now on
 * @CLAWT_OAUTH_POLL_DENIED: the person said no
 * @CLAWT_OAUTH_POLL_EXPIRED: they took too long
 * @CLAWT_OAUTH_POLL_FAILED: anything else
 *
 * What one poll of the token endpoint means.
 *
 * The distinction that matters is between %CLAWT_OAUTH_POLL_PENDING and
 * %CLAWT_OAUTH_POLL_FAILED, and it is not visible in the HTTP status.
 * A provider answers a poll for a code nobody has entered yet with
 * **400 Bad Request** and `error: authorization_pending` in the body --
 * so a client that reads the status and stops has built a device flow
 * that can never once succeed, while looking entirely correct.
 */
typedef enum {
    CLAWT_OAUTH_POLL_GRANTED = 0,
    CLAWT_OAUTH_POLL_PENDING,
    CLAWT_OAUTH_POLL_SLOW_DOWN,
    CLAWT_OAUTH_POLL_DENIED,
    CLAWT_OAUTH_POLL_EXPIRED,
    CLAWT_OAUTH_POLL_FAILED
} ClawtOauthPollResult;

/**
 * clawt_oauth_read_poll:
 * @json: the token endpoint's response body
 * @length: its length, or -1 if NUL-terminated
 * @now: the current time in seconds
 * @out_token: (out) (transfer full) (optional): the token, when granted
 * @out_message: (out) (transfer full) (optional): what went wrong, otherwise
 *
 * Classifies one poll response.
 *
 * Returns: what the response means
 */
ClawtOauthPollResult clawt_oauth_read_poll(const gchar      *json,
                                           gssize            length,
                                           gint64            now,
                                           ClawtOauthToken **out_token,
                                           gchar           **out_message);

/**
 * clawt_oauth_device_begin_async:
 * @auth_url: the device authorization endpoint
 * @client_id: the application registered with the provider
 * @scopes: (nullable): what to ask for, space separated
 * @cancellable: (nullable): a #GCancellable
 * @callback: called when the codes arrive
 * @user_data: for @callback
 *
 * Asks the provider to start a device flow.
 *
 * Returns nothing that can be used yet: the caller must show the user
 * code and then call clawt_oauth_device_poll_async().
 */
void clawt_oauth_device_begin_async(const gchar         *auth_url,
                                    const gchar         *client_id,
                                    const gchar         *scopes,
                                    GCancellable        *cancellable,
                                    GAsyncReadyCallback  callback,
                                    gpointer             user_data);

/**
 * clawt_oauth_device_begin_finish:
 * @result: the #GAsyncResult
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the codes, or %NULL
 */
ClawtDeviceCode *clawt_oauth_device_begin_finish(GAsyncResult  *result,
                                                 GError       **error);

/**
 * clawt_oauth_device_poll_async:
 * @token_url: the token endpoint
 * @client_id: the application
 * @client_secret: (nullable): if the provider insists on one
 * @code: what clawt_oauth_device_begin_finish() returned
 * @cancellable: (nullable): a #GCancellable
 * @callback: called once, when the flow settles
 * @user_data: for @callback
 *
 * Polls until the person approves, refuses, or runs out of time.
 *
 * Honours the provider's interval, and lengthens it permanently on a
 * `slow_down` -- a provider that has asked once to be polled less often
 * will ask again, and a client that reverts to the old interval after a
 * single slower poll gets rate limited out of a flow the person is in
 * the middle of completing.
 */
void clawt_oauth_device_poll_async(const gchar         *token_url,
                                   const gchar         *client_id,
                                   const gchar         *client_secret,
                                   ClawtDeviceCode     *code,
                                   GCancellable        *cancellable,
                                   GAsyncReadyCallback  callback,
                                   gpointer             user_data);

/**
 * clawt_oauth_device_poll_finish:
 * @result: the #GAsyncResult
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the token, or %NULL
 */
ClawtOauthToken *clawt_oauth_device_poll_finish(GAsyncResult  *result,
                                                GError       **error);

/**
 * clawt_oauth_pkce_verifier:
 *
 * A fresh PKCE code verifier.
 *
 * Drawn from the kernel's random pool and never from GLib's, which is a
 * Mersenne Twister and predictable from its output.  A predictable
 * verifier is not a weaker PKCE; it is no PKCE, because the whole
 * mechanism is that only the client that started the flow can finish it.
 * Failing loudly beats falling back.
 *
 * Returns: (transfer full) (nullable): 43 unreserved characters, or %NULL
 *   if the system has no usable randomness
 */
gchar *clawt_oauth_pkce_verifier(void);

/**
 * clawt_oauth_pkce_challenge:
 * @verifier: a code verifier
 *
 * The S256 challenge for @verifier: base64url of its SHA-256, unpadded.
 *
 * Returns: (transfer full): the challenge
 */
gchar *clawt_oauth_pkce_challenge(const gchar *verifier);

/**
 * clawt_oauth_authorize_url:
 * @auth_url: the provider's authorization endpoint
 * @client_id: the application
 * @redirect_uri: where the provider sends the browser back
 * @scopes: (nullable): what to ask for
 * @state: an unguessable value echoed back, to bind the reply to this attempt
 * @challenge: (nullable): the PKCE challenge
 *
 * Builds the URL a person opens to approve the request.
 *
 * Returns: (transfer full): the URL
 */
gchar *clawt_oauth_authorize_url(const gchar *auth_url,
                                 const gchar *client_id,
                                 const gchar *redirect_uri,
                                 const gchar *scopes,
                                 const gchar *state,
                                 const gchar *challenge);

/**
 * clawt_oauth_parse_redirect:
 * @target: an HTTP request target, such as `/callback?code=x&state=y`
 * @out_code: (out) (transfer full) (optional): the authorization code
 * @out_state: (out) (transfer full) (optional): the state echoed back
 * @out_error: (out) (transfer full) (optional): the provider's refusal
 *
 * Reads what a provider sent back to the loopback listener.
 *
 * A plain function so the parsing can be asserted on without a browser,
 * a listening socket or a provider -- the three things that make this
 * path awkward to reach and easy to get subtly wrong.
 *
 * Returns: %TRUE if @target carried either a code or an error
 */
gboolean clawt_oauth_parse_redirect(const gchar  *target,
                                    gchar       **out_code,
                                    gchar       **out_state,
                                    gchar       **out_error);

/**
 * clawt_oauth_await_redirect_async:
 * @port: the loopback port to listen on
 * @expected_state: what the provider must echo back
 * @timeout_seconds: how long to wait before giving up
 * @cancellable: (nullable): a #GCancellable
 * @callback: called when a code arrives or time runs out
 * @user_data: for @callback
 *
 * Listens on 127.0.0.1 for the provider's redirect.
 *
 * Binds the loopback address only.  A redirect carries an authorization
 * code in a URL, and a listener on every interface offers that code to
 * whoever on the network reaches the port first.
 */
void clawt_oauth_await_redirect_async(guint                port,
                                      const gchar         *expected_state,
                                      guint                timeout_seconds,
                                      GCancellable        *cancellable,
                                      GAsyncReadyCallback  callback,
                                      gpointer             user_data);

/**
 * clawt_oauth_await_redirect_finish:
 * @result: the #GAsyncResult
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the authorization code, or %NULL
 */
gchar *clawt_oauth_await_redirect_finish(GAsyncResult  *result,
                                         GError       **error);

/**
 * clawt_oauth_exchange_async:
 * @token_url: the token endpoint
 * @client_id: the application
 * @client_secret: (nullable): if the provider insists on one
 * @code: the authorization code
 * @redirect_uri: the same one the authorization request carried
 * @verifier: (nullable): the PKCE verifier
 * @cancellable: (nullable): a #GCancellable
 * @callback: called with the token
 * @user_data: for @callback
 *
 * Exchanges an authorization code for a token.
 */
void clawt_oauth_exchange_async(const gchar         *token_url,
                                const gchar         *client_id,
                                const gchar         *client_secret,
                                const gchar         *code,
                                const gchar         *redirect_uri,
                                const gchar         *verifier,
                                GCancellable        *cancellable,
                                GAsyncReadyCallback  callback,
                                gpointer             user_data);

/**
 * clawt_oauth_exchange_finish:
 * @result: the #GAsyncResult
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the token, or %NULL
 */
ClawtOauthToken *clawt_oauth_exchange_finish(GAsyncResult  *result,
                                             GError       **error);

/**
 * clawt_oauth_refresh_async:
 * @token_url: the token endpoint
 * @client_id: the application
 * @client_secret: (nullable): if the provider insists on one
 * @refresh_token: what to renew with
 * @cancellable: (nullable): a #GCancellable
 * @callback: called with the new token
 * @user_data: for @callback
 *
 * Renews an access token.
 */
void clawt_oauth_refresh_async(const gchar         *token_url,
                               const gchar         *client_id,
                               const gchar         *client_secret,
                               const gchar         *refresh_token,
                               GCancellable        *cancellable,
                               GAsyncReadyCallback  callback,
                               gpointer             user_data);

/**
 * clawt_oauth_refresh_finish:
 * @result: the #GAsyncResult
 * @error: (out) (optional): return location for a #GError
 *
 * A provider that issues a new refresh token returns it; one that does
 * not leaves @refresh_token empty, and the caller must keep the old one
 * rather than storing the blank.  Losing a refresh token means the
 * person has to authorise again for no reason they can see.
 *
 * Returns: (transfer full) (nullable): the renewed token, or %NULL
 */
ClawtOauthToken *clawt_oauth_refresh_finish(GAsyncResult  *result,
                                            GError       **error);

/**
 * clawt_oauth_revoke_async:
 * @revoke_url: the provider's revocation endpoint
 * @client_id: the application
 * @client_secret: (nullable): if the provider insists on one
 * @token: the credential to hand back
 * @cancellable: (nullable): a #GCancellable
 * @callback: called when the provider has answered
 * @user_data: for @callback
 *
 * Tells the provider to stop honouring @token.
 *
 * Deleting our copy is not revocation.  A token we forgot is a token
 * that still works for anybody who has it, for as long as the provider
 * says -- which for a refresh token can be months.  Where a provider
 * offers no revocation endpoint the only honest thing is to say so and
 * point at their settings page, which is what the caller does.
 */
void clawt_oauth_revoke_async(const gchar         *revoke_url,
                              const gchar         *client_id,
                              const gchar         *client_secret,
                              const gchar         *token,
                              GCancellable        *cancellable,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data);

/**
 * clawt_oauth_revoke_finish:
 * @result: the #GAsyncResult
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE if the provider accepted the revocation
 */
gboolean clawt_oauth_revoke_finish(GAsyncResult *result, GError **error);

G_END_DECLS
