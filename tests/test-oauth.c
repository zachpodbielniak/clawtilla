/*
 * test-oauth.c - The flows that obtain a credential
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Everything that reads a wire format is tested here without a socket,
 * which is most of what can go wrong: a token response arrives once,
 * from a server nobody controls, and a mistake in reading it is
 * otherwise only reproducible by starting a whole flow again.
 *
 * The two tests that do speak HTTP stand up a provider of their own on
 * loopback and sit behind CLAWT_TEST_INTEGRATION, because `make test`
 * opens no sockets.  They are the ones that prove the poll loop
 * terminates -- the classification is pure and covered above, but a
 * device flow can classify every response perfectly and still never
 * finish if the timer is wrong.
 */

#include "clawtilla.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <sys/stat.h>

static gboolean
integration_enabled(void)
{
    return g_getenv("CLAWT_TEST_INTEGRATION") != NULL;
}

/* ── Reading a token ─────────────────────────────────────────────── */

static void
test_a_token_is_read(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtOauthToken) token = NULL;
    const gchar *json =
        "{\"access_token\":\"gho_abc\",\"token_type\":\"bearer\","
        "\"scope\":\"repo read:org\",\"refresh_token\":\"ghr_xyz\","
        "\"expires_in\":3600}";

    token = clawt_oauth_token_parse(json, -1, 1000, &error);

    g_assert_no_error(error);
    g_assert_nonnull(token);
    g_assert_cmpstr(token->access_token, ==, "gho_abc");
    g_assert_cmpstr(token->refresh_token, ==, "ghr_xyz");
    g_assert_cmpstr(token->scopes, ==, "repo read:org");
    g_assert_cmpint(token->expires_at, ==, 4600);
}

/*
 * The field on the wire is a duration and the field we keep is an
 * instant.  Storing the duration as it came makes every restart look
 * like a fresh hour, so a token that expired overnight reads as valid
 * each morning and fails on the first call made with it.
 */
static void
test_expiry_is_absolute_not_a_duration(void)
{
    g_autoptr(ClawtOauthToken) token = NULL;

    token = clawt_oauth_token_parse("{\"access_token\":\"a\","
                                    "\"expires_in\":60}", -1, 5000, NULL);

    g_assert_nonnull(token);
    g_assert_cmpint(token->expires_at, ==, 5060);

    g_assert_false(clawt_oauth_token_is_expired(token, 5000, 0));
    g_assert_true(clawt_oauth_token_is_expired(token, 5061, 0));

    /* And the headroom is what keeps a call from expiring in flight. */
    g_assert_false(clawt_oauth_token_is_expired(token, 5000, 30));
    g_assert_true(clawt_oauth_token_is_expired(token, 5040, 30));
}

/*
 * A token with no expires_in does not expire.  Treating a missing field
 * as zero would make every such token permanently stale and send the
 * refresher into a loop against a provider that has nothing to refresh.
 */
static void
test_a_token_without_an_expiry_never_expires(void)
{
    g_autoptr(ClawtOauthToken) token = NULL;

    token = clawt_oauth_token_parse("{\"access_token\":\"a\"}", -1, 5000,
                                    NULL);

    g_assert_nonnull(token);
    g_assert_cmpint(token->expires_at, ==, 0);
    g_assert_false(clawt_oauth_token_is_expired(token, G_MAXINT64 - 1, 0));
}

/* Providers send this both ways, and reading only one loses the value. */
static void
test_a_numeric_field_may_arrive_as_a_string(void)
{
    g_autoptr(ClawtOauthToken) token = NULL;

    token = clawt_oauth_token_parse("{\"access_token\":\"a\","
                                    "\"expires_in\":\"3600\"}", -1, 0, NULL);

    g_assert_nonnull(token);
    g_assert_cmpint(token->expires_at, ==, 3600);
}

static void
test_a_response_with_no_token_is_an_error(void)
{
    g_autoptr(GError) error = NULL;
    ClawtOauthToken *token;

    token = clawt_oauth_token_parse("{\"token_type\":\"bearer\"}", -1, 0,
                                    &error);

    g_assert_null(token);
    g_assert_nonnull(error);
}

static void
test_a_token_survives_a_round_trip_through_a_file(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-oauth-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(dir, "token.json", NULL);
    g_autoptr(ClawtOauthToken) written = NULL;
    g_autoptr(ClawtOauthToken) read_back = NULL;
    g_autoptr(GError) error = NULL;
    struct stat info;

    written = clawt_oauth_token_parse("{\"access_token\":\"a\","
                                      "\"refresh_token\":\"r\","
                                      "\"scope\":\"x y\","
                                      "\"expires_in\":100}", -1, 900, NULL);

    g_assert_true(clawt_oauth_token_save(written, path, &error));
    g_assert_no_error(error);

    /* A credential file that anyone can read is not a credential file. */
    g_assert_cmpint(stat(path, &info), ==, 0);
    g_assert_cmpint(info.st_mode & 0777, ==, 0600);

    read_back = clawt_oauth_token_load(path, &error);

    g_assert_no_error(error);
    g_assert_nonnull(read_back);
    g_assert_cmpstr(read_back->access_token, ==, "a");
    g_assert_cmpstr(read_back->refresh_token, ==, "r");
    g_assert_cmpstr(read_back->scopes, ==, "x y");

    /*
     * The instant, not the duration.  A file that stored `expires_in`
     * would read back as valid for another 100 seconds however long ago
     * it was written.
     */
    g_assert_cmpint(read_back->expires_at, ==, 1000);

    g_unlink(path);
    g_rmdir(dir);
}

/* ── Device codes ────────────────────────────────────────────────── */

static void
test_a_device_code_is_read(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtDeviceCode) code = NULL;
    const gchar *json =
        "{\"device_code\":\"dc-secret\",\"user_code\":\"WDJB-MJHT\","
        "\"verification_uri\":\"https://github.com/login/device\","
        "\"expires_in\":900,\"interval\":5}";

    code = clawt_oauth_parse_device_code(json, -1, 100, &error);

    g_assert_no_error(error);
    g_assert_nonnull(code);
    g_assert_cmpstr(code->user_code, ==, "WDJB-MJHT");
    g_assert_cmpstr(code->device_code, ==, "dc-secret");
    g_assert_cmpint(code->interval, ==, 5);
    g_assert_cmpint(code->expires_at, ==, 1000);
}

/*
 * Some providers still send the draft's spelling.  Accepting only one
 * leaves the person holding a code and no page to type it into.
 */
static void
test_either_spelling_of_the_verification_page_is_accepted(void)
{
    g_autoptr(ClawtDeviceCode) code = NULL;

    code = clawt_oauth_parse_device_code(
        "{\"device_code\":\"d\",\"user_code\":\"U\","
        "\"verification_url\":\"https://example.test/activate\"}", -1, 0,
        NULL);

    g_assert_nonnull(code);
    g_assert_cmpstr(code->verification_uri, ==, "https://example.test/activate");
}

/* A zero or missing interval must not become a busy loop. */
static void
test_a_missing_interval_gets_a_sane_one(void)
{
    g_autoptr(ClawtDeviceCode) code = NULL;

    code = clawt_oauth_parse_device_code(
        "{\"device_code\":\"d\",\"user_code\":\"U\","
        "\"verification_uri\":\"https://example.test/\",\"interval\":0}",
        -1, 0, NULL);

    g_assert_nonnull(code);
    g_assert_cmpint(code->interval, >=, 1);
}

static void
test_a_refusal_to_start_a_flow_is_reported(void)
{
    g_autoptr(GError) error = NULL;
    ClawtDeviceCode *code;

    code = clawt_oauth_parse_device_code(
        "{\"error\":\"unauthorized_client\","
        "\"error_description\":\"device flow is not enabled\"}", -1, 0,
        &error);

    g_assert_null(code);
    g_assert_nonnull(error);
    g_assert_nonnull(g_strstr_len(error->message, -1,
                                  "device flow is not enabled"));
}

/* ── Classifying a poll ──────────────────────────────────────────── */

/*
 * The one that decides whether a device flow can ever succeed.
 *
 * A provider answers a poll for a code nobody has entered yet with 400
 * Bad Request and `authorization_pending` in the body.  Read as an HTTP
 * failure that is a terminal error on the very first poll, and the flow
 * never once completes while looking entirely reasonable.
 */
static void
test_pending_is_not_a_failure(void)
{
    g_autofree gchar *message = NULL;
    ClawtOauthToken *token = NULL;

    g_assert_cmpint(clawt_oauth_read_poll(
                        "{\"error\":\"authorization_pending\"}", -1, 0,
                        &token, &message),
                    ==, CLAWT_OAUTH_POLL_PENDING);

    g_assert_null(token);
}

static void
test_each_poll_outcome_is_told_apart(void)
{
    struct {
        const gchar         *body;
        ClawtOauthPollResult expected;
    } cases[] = {
        { "{\"access_token\":\"a\"}",            CLAWT_OAUTH_POLL_GRANTED },
        { "{\"error\":\"authorization_pending\"}", CLAWT_OAUTH_POLL_PENDING },
        { "{\"error\":\"slow_down\"}",           CLAWT_OAUTH_POLL_SLOW_DOWN },
        { "{\"error\":\"access_denied\"}",       CLAWT_OAUTH_POLL_DENIED },
        { "{\"error\":\"expired_token\"}",       CLAWT_OAUTH_POLL_EXPIRED },
        { "{\"error\":\"invalid_grant\"}",       CLAWT_OAUTH_POLL_FAILED },
        { "not json at all",                     CLAWT_OAUTH_POLL_FAILED }
    };
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(cases); i++) {
        g_autofree gchar *message = NULL;
        ClawtOauthToken *token = NULL;

        g_assert_cmpint(clawt_oauth_read_poll(cases[i].body, -1, 0, &token,
                                              &message),
                        ==, cases[i].expected);

        clawt_oauth_token_free(token);
    }
}

/* ── PKCE ────────────────────────────────────────────────────────── */

/*
 * The published vector from RFC 7636 appendix B.  Asserting against our
 * own output would pass just as happily with the padding left on, or
 * with base64 rather than base64url -- both of which every provider
 * rejects, and neither of which is visible by looking at the string.
 */
static void
test_the_challenge_matches_the_published_vector(void)
{
    g_autofree gchar *challenge = NULL;

    challenge = clawt_oauth_pkce_challenge(
        "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk");

    g_assert_cmpstr(challenge, ==,
                    "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
}

static void
test_a_verifier_is_unreserved_and_long_enough(void)
{
    g_autofree gchar *first = clawt_oauth_pkce_verifier();
    g_autofree gchar *second = clawt_oauth_pkce_verifier();
    const gchar *p;

    g_assert_nonnull(first);
    g_assert_nonnull(second);

    /* RFC 7636 asks for 43 to 128 characters. */
    g_assert_cmpint((gint)strlen(first), >=, 43);
    g_assert_cmpint((gint)strlen(first), <=, 128);

    /* Two in a row being equal would mean it is not random at all. */
    g_assert_cmpstr(first, !=, second);

    for (p = first; *p != '\0'; p++)
        g_assert_true(g_ascii_isalnum(*p) || *p == '-' || *p == '.' ||
                      *p == '_' || *p == '~');
}

static void
test_an_authorize_url_keeps_a_query_the_endpoint_already_had(void)
{
    g_autofree gchar *plain = NULL;
    g_autofree gchar *tenanted = NULL;

    plain = clawt_oauth_authorize_url("https://example.test/authorize", "cid",
                                      "http://127.0.0.1:8765/callback",
                                      "read", "st4te", "ch4llenge");

    g_assert_nonnull(g_strstr_len(plain, -1, "/authorize?response_type=code"));
    g_assert_nonnull(g_strstr_len(plain, -1, "code_challenge_method=S256"));
    g_assert_nonnull(g_strstr_len(plain, -1, "state=st4te"));

    /* The redirect must survive escaping intact. */
    g_assert_nonnull(g_strstr_len(plain, -1,
                                  "redirect_uri=http%3A%2F%2F127.0.0.1"
                                  "%3A8765%2Fcallback"));

    tenanted = clawt_oauth_authorize_url("https://example.test/authorize?t=1",
                                         "cid", "http://127.0.0.1/cb", NULL,
                                         "s", NULL);

    /* Appended, not replaced -- and no PKCE method without a challenge. */
    g_assert_nonnull(g_strstr_len(tenanted, -1, "?t=1&response_type=code"));
    g_assert_null(g_strstr_len(tenanted, -1, "code_challenge"));
}

/* ── The redirect ────────────────────────────────────────────────── */

static void
test_a_redirect_is_read(void)
{
    g_autofree gchar *code = NULL;
    g_autofree gchar *state = NULL;
    g_autofree gchar *failure = NULL;

    g_assert_true(clawt_oauth_parse_redirect("/callback?code=abc&state=xyz",
                                             &code, &state, &failure));

    g_assert_cmpstr(code, ==, "abc");
    g_assert_cmpstr(state, ==, "xyz");
    g_assert_null(failure);
}

static void
test_a_refusal_comes_back_through_the_redirect(void)
{
    g_autofree gchar *code = NULL;
    g_autofree gchar *state = NULL;
    g_autofree gchar *failure = NULL;

    g_assert_true(clawt_oauth_parse_redirect("/callback?error=access_denied",
                                             &code, &state, &failure));

    g_assert_null(code);
    g_assert_cmpstr(failure, ==, "access_denied");
}

/*
 * A browser fetches /favicon.ico of its own accord.  Treating any
 * request as the redirect would finish the flow with no code at all,
 * and the failure would look like the provider's fault.
 */
static void
test_a_request_that_is_not_the_redirect_is_not_one(void)
{
    g_assert_false(clawt_oauth_parse_redirect("/favicon.ico", NULL, NULL,
                                              NULL));
    g_assert_false(clawt_oauth_parse_redirect("/callback", NULL, NULL, NULL));
    g_assert_false(clawt_oauth_parse_redirect(NULL, NULL, NULL, NULL));
}

/*
 * Percent escapes and the query form's `+` are different spellings of
 * the same thing and both appear.  A state that fails to match by one
 * character is rejected as somebody else's reply.
 */
static void
test_escaping_in_a_redirect_is_undone(void)
{
    g_autofree gchar *code = NULL;
    g_autofree gchar *state = NULL;

    g_assert_true(clawt_oauth_parse_redirect(
        "/callback?code=a%2Fb%2Bc&state=one+two", &code, &state, NULL));

    g_assert_cmpstr(code, ==, "a/b+c");
    g_assert_cmpstr(state, ==, "one two");
}

/* ── A whole device flow, against a provider of our own ──────────── */

typedef struct {
    SoupServer *server;
    gchar      *base;
    guint       polls;
    GMainLoop  *loop;
} FakeProvider;

static void
handle_device(SoupServer *server, SoupServerMessage *message,
              const gchar *path, GHashTable *query, gpointer user_data)
{
    const gchar *body =
        "{\"device_code\":\"dc-secret\",\"user_code\":\"WDJB-MJHT\","
        "\"verification_uri\":\"https://example.test/device\","
        "\"expires_in\":300,\"interval\":1}";

    soup_server_message_set_status(message, SOUP_STATUS_OK, NULL);
    soup_server_message_set_response(message, "application/json",
                                     SOUP_MEMORY_COPY, body, strlen(body));
}

/*
 * Answers the way a real provider does: pending while nobody has typed
 * the code, one slow_down along the way, and the grant at the end --
 * each of them a 400 except the last.
 */
static void
handle_token(SoupServer *server, SoupServerMessage *message,
             const gchar *path, GHashTable *query, gpointer user_data)
{
    FakeProvider *fake = user_data;
    const gchar *body;
    guint status;

    fake->polls++;

    if (fake->polls == 1) {
        body = "{\"error\":\"authorization_pending\"}";
        status = SOUP_STATUS_BAD_REQUEST;
    } else if (fake->polls == 2) {
        body = "{\"error\":\"slow_down\"}";
        status = SOUP_STATUS_BAD_REQUEST;
    } else {
        body = "{\"access_token\":\"granted-at-last\","
               "\"refresh_token\":\"r\",\"token_type\":\"bearer\","
               "\"expires_in\":3600}";
        status = SOUP_STATUS_OK;
    }

    soup_server_message_set_status(message, status, NULL);
    soup_server_message_set_response(message, "application/json",
                                     SOUP_MEMORY_COPY, body, strlen(body));
}

static FakeProvider *
fake_provider_new(void)
{
    FakeProvider *fake = g_new0(FakeProvider, 1);
    g_autoptr(GError) error = NULL;
    GSList *uris;

    fake->server = soup_server_new(NULL, NULL);

    soup_server_add_handler(fake->server, "/device", handle_device, fake,
                            NULL);
    soup_server_add_handler(fake->server, "/token", handle_token, fake, NULL);

    if (!soup_server_listen_local(fake->server, 0, 0, &error))
        g_error("cannot listen for the fake provider: %s", error->message);

    uris = soup_server_get_uris(fake->server);
    fake->base = g_uri_to_string(uris->data);
    g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);

    /* g_uri_to_string() gives a trailing slash we would double up on. */
    if (g_str_has_suffix(fake->base, "/"))
        fake->base[strlen(fake->base) - 1] = '\0';

    fake->loop = g_main_loop_new(NULL, FALSE);

    return fake;
}

static void
fake_provider_free(FakeProvider *fake)
{
    soup_server_disconnect(fake->server);
    g_object_unref(fake->server);
    g_main_loop_unref(fake->loop);
    g_free(fake->base);
    g_free(fake);
}

static ClawtOauthToken *device_result = NULL;
static GError          *device_error = NULL;

static void
on_polled(GObject *source, GAsyncResult *result, gpointer user_data)
{
    FakeProvider *fake = user_data;

    device_result = clawt_oauth_device_poll_finish(result, &device_error);
    g_main_loop_quit(fake->loop);
}

static void
on_begun(GObject *source, GAsyncResult *result, gpointer user_data)
{
    FakeProvider *fake = user_data;
    g_autoptr(ClawtDeviceCode) code = NULL;
    g_autofree gchar *token_url = NULL;

    code = clawt_oauth_device_begin_finish(result, &device_error);

    if (code == NULL) {
        g_main_loop_quit(fake->loop);
        return;
    }

    g_assert_cmpstr(code->user_code, ==, "WDJB-MJHT");

    token_url = g_strconcat(fake->base, "/token", NULL);

    clawt_oauth_device_poll_async(token_url, "cid", NULL, code, NULL,
                                  on_polled, fake);
}

/*
 * The test that proves the loop terminates.
 *
 * Everything it exercises is invisible to the pure tests above: that a
 * 400 does not stop the flow, that the timer re-arms, that a slow_down
 * lengthens the interval without abandoning the attempt, and that the
 * grant is recognised when it finally comes.
 */
static void
test_a_device_flow_runs_to_a_grant(void)
{
    FakeProvider *fake;
    g_autofree gchar *device_url = NULL;

    if (!integration_enabled()) {
        g_test_skip("needs CLAWT_TEST_INTEGRATION; opens a loopback socket");
        return;
    }

    fake = fake_provider_new();
    device_url = g_strconcat(fake->base, "/device", NULL);

    clawt_oauth_device_begin_async(device_url, "cid", "repo", NULL, on_begun,
                                   fake);

    g_main_loop_run(fake->loop);

    g_assert_no_error(device_error);
    g_assert_nonnull(device_result);
    g_assert_cmpstr(device_result->access_token, ==, "granted-at-last");

    /* Three polls: pending, slow_down, granted. */
    g_assert_cmpint(fake->polls, ==, 3);

    g_clear_pointer(&device_result, clawt_oauth_token_free);
    g_clear_error(&device_error);
    fake_provider_free(fake);
}

static void
on_refreshed(GObject *source, GAsyncResult *result, gpointer user_data)
{
    FakeProvider *fake = user_data;

    device_result = clawt_oauth_refresh_finish(result, &device_error);
    g_main_loop_quit(fake->loop);
}

static void
test_a_token_is_renewed(void)
{
    FakeProvider *fake;
    g_autofree gchar *token_url = NULL;

    if (!integration_enabled()) {
        g_test_skip("needs CLAWT_TEST_INTEGRATION; opens a loopback socket");
        return;
    }

    fake = fake_provider_new();

    /* Skip past the pending answers; a refresh is granted at once. */
    fake->polls = 2;

    token_url = g_strconcat(fake->base, "/token", NULL);

    clawt_oauth_refresh_async(token_url, "cid", NULL, "old-refresh", NULL,
                              on_refreshed, fake);

    g_main_loop_run(fake->loop);

    g_assert_no_error(device_error);
    g_assert_nonnull(device_result);
    g_assert_cmpstr(device_result->access_token, ==, "granted-at-last");

    g_clear_pointer(&device_result, clawt_oauth_token_free);
    g_clear_error(&device_error);
    fake_provider_free(fake);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/oauth/token-parse", test_a_token_is_read);
    g_test_add_func("/oauth/token-expiry-absolute",
                    test_expiry_is_absolute_not_a_duration);
    g_test_add_func("/oauth/token-no-expiry",
                    test_a_token_without_an_expiry_never_expires);
    g_test_add_func("/oauth/token-numeric-string",
                    test_a_numeric_field_may_arrive_as_a_string);
    g_test_add_func("/oauth/token-missing",
                    test_a_response_with_no_token_is_an_error);
    g_test_add_func("/oauth/token-round-trip",
                    test_a_token_survives_a_round_trip_through_a_file);

    g_test_add_func("/oauth/device-code", test_a_device_code_is_read);
    g_test_add_func("/oauth/device-code-url-spelling",
                    test_either_spelling_of_the_verification_page_is_accepted);
    g_test_add_func("/oauth/device-code-interval",
                    test_a_missing_interval_gets_a_sane_one);
    g_test_add_func("/oauth/device-code-refused",
                    test_a_refusal_to_start_a_flow_is_reported);

    g_test_add_func("/oauth/poll-pending-is-not-failure",
                    test_pending_is_not_a_failure);
    g_test_add_func("/oauth/poll-outcomes",
                    test_each_poll_outcome_is_told_apart);

    g_test_add_func("/oauth/pkce-vector",
                    test_the_challenge_matches_the_published_vector);
    g_test_add_func("/oauth/pkce-verifier",
                    test_a_verifier_is_unreserved_and_long_enough);
    g_test_add_func("/oauth/authorize-url",
                    test_an_authorize_url_keeps_a_query_the_endpoint_already_had);

    g_test_add_func("/oauth/redirect", test_a_redirect_is_read);
    g_test_add_func("/oauth/redirect-refused",
                    test_a_refusal_comes_back_through_the_redirect);
    g_test_add_func("/oauth/redirect-not-ours",
                    test_a_request_that_is_not_the_redirect_is_not_one);
    g_test_add_func("/oauth/redirect-escaping",
                    test_escaping_in_a_redirect_is_undone);

    g_test_add_func("/oauth/device-flow-completes",
                    test_a_device_flow_runs_to_a_grant);
    g_test_add_func("/oauth/refresh", test_a_token_is_renewed);

    return g_test_run();
}
