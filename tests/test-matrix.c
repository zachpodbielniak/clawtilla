/*
 * test-matrix.c - Homeserver addresses, and what a homeserver sends back
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * These are the pure halves of the Matrix code, which is most of what
 * can go wrong with it.  The error shape matters as much as the success
 * one: a wrong password is an ordinary outcome, and it has to reach the
 * person as the server's own words rather than as "login failed (403)".
 */

#include <clawtilla.h>
#include <libsoup/soup.h>

#include <string.h>

/* ── Addresses ───────────────────────────────────────────────────── */

static void
check_url(const gchar *typed, const gchar *want)
{
    g_autofree gchar *got = clawt_matrix_base_url(typed);

    if (want == NULL) {
        g_assert_null(got);
        return;
    }

    g_assert_cmpstr(got, ==, want);
}

/*
 * People type all of these and mean the same server, so all of them have
 * to work -- the alternative is an error message about a URL that looks
 * perfectly correct to whoever typed it.
 */
static void
test_the_forms_people_type(void)
{
    check_url("matrix.example.org", "https://matrix.example.org");
    check_url("https://matrix.example.org", "https://matrix.example.org");
    check_url("https://matrix.example.org/", "https://matrix.example.org");
    check_url("https://matrix.example.org///", "https://matrix.example.org");
    check_url("http://127.0.0.1:8008", "http://127.0.0.1:8008");
    check_url("matrix.example.org:8448", "https://matrix.example.org:8448");
}

/*
 * A path is refused rather than kept or stripped.  Keeping it gives
 * `/_matrix/_matrix/...` to whoever pasted the API root; stripping it
 * silently ignores a server genuinely hosted under a prefix.
 */
static void
test_a_path_is_refused(void)
{
    check_url("https://matrix.example.org/_matrix", NULL);
    check_url("https://example.org/matrix/", NULL);
}

static void
test_nonsense_is_refused(void)
{
    check_url(NULL, NULL);
    check_url("", NULL);
    check_url("ftp://matrix.example.org", NULL);
    check_url("https://", NULL);
}

/* ── Login ───────────────────────────────────────────────────────── */

static void
test_a_successful_login_is_read(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtMatrixLogin) login = NULL;

    login = clawt_matrix_parse_login(
        "{\"user_id\":\"@agent:example.org\","
        "\"access_token\":\"syt_abc\","
        "\"device_id\":\"ABCDEF\","
        "\"home_server\":\"example.org\"}", &error);

    g_assert_no_error(error);
    g_assert_nonnull(login);
    g_assert_cmpstr(login->user_id, ==, "@agent:example.org");
    g_assert_cmpstr(login->access_token, ==, "syt_abc");
    g_assert_cmpstr(login->device_id, ==, "ABCDEF");
}

/*
 * The server's own message is what a person can act on.  "Invalid
 * password" tells them what to do; a status code does not.
 */
static void
test_the_servers_own_words_survive(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtMatrixLogin) login = NULL;

    login = clawt_matrix_parse_login(
        "{\"errcode\":\"M_FORBIDDEN\",\"error\":\"Invalid password\"}",
        &error);

    g_assert_null(login);
    g_assert_nonnull(error);
    g_assert_nonnull(strstr(error->message, "Invalid password"));
    g_assert_nonnull(strstr(error->message, "M_FORBIDDEN"));
}

/*
 * Some deployments answer 200 with an errcode, which would otherwise
 * become a login with no token that fails much later and elsewhere.
 */
static void
test_an_errcode_is_an_error_whatever_the_status(void)
{
    g_autoptr(GError) error = NULL;

    g_assert_null(clawt_matrix_parse_login(
        "{\"errcode\":\"M_LIMIT_EXCEEDED\",\"error\":\"Too many requests\","
        "\"retry_after_ms\":4000}", &error));
    g_assert_nonnull(error);
}

static void
test_a_login_with_no_token_is_refused(void)
{
    g_autoptr(GError) error = NULL;

    g_assert_null(clawt_matrix_parse_login(
        "{\"user_id\":\"@agent:example.org\"}", &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL);
}

static void
test_rubbish_is_refused(void)
{
    g_autoptr(GError) error = NULL;

    g_assert_null(clawt_matrix_parse_login(NULL, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);

    g_assert_null(clawt_matrix_parse_login("<html>403 Forbidden</html>",
                                           &error));
    g_assert_nonnull(error);
    g_clear_error(&error);

    g_assert_null(clawt_matrix_parse_login("[1, 2, 3]", &error));
    g_assert_nonnull(error);
}

/* ── Rooms ───────────────────────────────────────────────────────── */

static void
test_joined_rooms_are_read(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GPtrArray) rooms = NULL;

    rooms = clawt_matrix_parse_joined_rooms(
        "{\"joined_rooms\":[\"!a:example.org\",\"!b:example.org\"]}", &error);

    g_assert_no_error(error);
    g_assert_nonnull(rooms);
    g_assert_cmpuint(rooms->len, ==, 2);
    g_assert_cmpstr(((ClawtMatrixRoom *)g_ptr_array_index(rooms, 0))->id, ==,
                    "!a:example.org");
}

static void
test_an_account_in_no_rooms_is_not_an_error(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GPtrArray) rooms = NULL;

    rooms = clawt_matrix_parse_joined_rooms("{\"joined_rooms\":[]}", &error);

    g_assert_no_error(error);
    g_assert_nonnull(rooms);
    g_assert_cmpuint(rooms->len, ==, 0);
}

static void
test_a_rejected_room_list_is_an_error(void)
{
    g_autoptr(GError) error = NULL;

    g_assert_null(clawt_matrix_parse_joined_rooms(
        "{\"errcode\":\"M_UNKNOWN_TOKEN\",\"error\":\"Invalid access token\"}",
        &error));
    g_assert_nonnull(error);
    g_assert_nonnull(strstr(error->message, "Invalid access token"));
    g_clear_error(&error);

    g_assert_null(clawt_matrix_parse_joined_rooms("{}", &error));
    g_assert_nonnull(error);
}

/*
 * The label is what a person picks from, so it falls back the way a
 * person would read it: the name, then the alias, then the id.
 */
static void
test_a_room_describes_itself(void)
{
    ClawtMatrixRoom room = { NULL, NULL, NULL };
    g_autofree gchar *by_id = NULL;
    g_autofree gchar *by_alias = NULL;
    g_autofree gchar *by_name = NULL;

    room.id = (gchar *)"!abc:example.org";
    by_id = clawt_matrix_room_describe(&room);
    g_assert_cmpstr(by_id, ==, "!abc:example.org");

    room.alias = (gchar *)"#ops:example.org";
    by_alias = clawt_matrix_room_describe(&room);
    g_assert_cmpstr(by_alias, ==, "#ops:example.org");

    room.name = (gchar *)"Fleet ops";
    by_name = clawt_matrix_room_describe(&room);
    g_assert_cmpstr(by_name, ==, "Fleet ops");
}

/* ── Sending ─────────────────────────────────────────────────────── */

/* A homeserver that keeps what it was sent, and answers @status. */
typedef struct {
    guint      status;
    GPtrArray *paths;   /* each request's method and path */
    gchar     *auth;
    gchar     *body;
} Homeserver;

static void
homeserver_serve(SoupServer *server, SoupServerMessage *message,
                 const char *path, GHashTable *query, gpointer user_data)
{
    Homeserver *hs = user_data;
    SoupMessageBody *body = soup_server_message_get_request_body(message);
    g_autoptr(GBytes) bytes = soup_message_body_flatten(body);
    const gchar *reply = hs->status == 200
        ? "{\"event_id\":\"$ev1\"}"
        : "{\"errcode\":\"M_FORBIDDEN\",\"error\":\"You are not in this room.\"}";

    (void)server;
    (void)query;
    g_ptr_array_add(hs->paths, g_strdup_printf("%s %s",
        soup_server_message_get_method(message), path));
    g_free(hs->auth);
    hs->auth = g_strdup(soup_message_headers_get_one(
        soup_server_message_get_request_headers(message), "Authorization"));
    g_free(hs->body);
    hs->body = g_strndup(g_bytes_get_data(bytes, NULL), g_bytes_get_size(bytes));
    soup_server_message_set_status(message, hs->status, NULL);
    soup_server_message_set_response(message, "application/json",
                                     SOUP_MEMORY_COPY, reply, strlen(reply));
}

typedef struct {
    gboolean  done;
    gboolean  ok;
    GError   *error;
} Sent;

static void
on_sent(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Sent *sent = user_data;

    (void)source;
    sent->ok = clawt_matrix_send_text_finish(result, &sent->error);
    sent->done = TRUE;
}

static void
send_and_wait(const gchar *base, const gchar *room, const gchar *seed,
              Sent *sent)
{
    clawt_matrix_send_text_async(base, "tok-123", room,
                                 "Summary: answered", seed, NULL, on_sent, sent);
    while (!sent->done)
        g_main_context_iteration(NULL, TRUE);
}

/*
 * A post is a PUT of an m.notice to the room, with the token as a
 * bearer header; the same seed is the same transaction, so a retry is
 * one message; and a refusal arrives in the server's own words.
 */
static void
test_a_notice_is_posted(void)
{
    Homeserver hs = { 200, g_ptr_array_new_with_free_func(g_free), NULL, NULL };
    g_autoptr(SoupServer) server = soup_server_new(NULL, NULL);
    g_autoptr(GError) error = NULL;
    g_autofree gchar *base = NULL;
    GSList *uris;
    Sent first = { FALSE, FALSE, NULL };
    Sent again = { FALSE, FALSE, NULL };
    Sent refused = { FALSE, FALSE, NULL };
    Sent bad_room = { FALSE, FALSE, NULL };

    soup_server_add_handler(server, NULL, homeserver_serve, &hs, NULL);
    g_assert_true(soup_server_listen_local(server, 0,
                                           SOUP_SERVER_LISTEN_IPV4_ONLY, &error));
    g_assert_no_error(error);
    uris = soup_server_get_uris(server);
    base = g_strdup_printf("http://127.0.0.1:%d", g_uri_get_port(uris->data));
    g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);

    send_and_wait(base, "!ops:example.org", "seed-1", &first);
    g_assert_no_error(first.error);
    g_assert_true(first.ok);
    g_assert_cmpuint(hs.paths->len, ==, 1);
    g_assert_true(g_str_has_prefix(g_ptr_array_index(hs.paths, 0),
        "PUT /_matrix/client/v3/rooms/!ops:example.org/send/m.room.message/clawt"));
    g_assert_cmpstr(hs.auth, ==, "Bearer tok-123");
    g_assert_nonnull(strstr(hs.body, "\"msgtype\":\"m.notice\""));
    g_assert_nonnull(strstr(hs.body, "\"body\":\"Summary: answered\""));

    send_and_wait(base, "!ops:example.org", "seed-1", &again);
    g_assert_true(again.ok);
    g_assert_cmpstr(g_ptr_array_index(hs.paths, 1), ==,
                    g_ptr_array_index(hs.paths, 0));

    hs.status = 403;
    send_and_wait(base, "!ops:example.org", "seed-2", &refused);
    g_assert_false(refused.ok);
    g_assert_nonnull(refused.error);
    g_assert_nonnull(strstr(refused.error->message, "not in this room"));

    /* Not a Matrix room id: nothing is sent. */
    send_and_wait(base, "dm:chief:w1", "seed-3", &bad_room);
    g_assert_false(bad_room.ok);
    g_assert_error(bad_room.error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
    g_assert_cmpuint(hs.paths->len, ==, 3);

    g_clear_error(&refused.error);
    g_clear_error(&bad_room.error);
    g_ptr_array_unref(hs.paths);
    g_free(hs.auth);
    g_free(hs.body);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/matrix/base-url", test_the_forms_people_type);
    g_test_add_func("/matrix/base-url-path", test_a_path_is_refused);
    g_test_add_func("/matrix/base-url-nonsense", test_nonsense_is_refused);
    g_test_add_func("/matrix/login", test_a_successful_login_is_read);
    g_test_add_func("/matrix/login-error",
                    test_the_servers_own_words_survive);
    g_test_add_func("/matrix/login-errcode-with-200",
                    test_an_errcode_is_an_error_whatever_the_status);
    g_test_add_func("/matrix/login-no-token",
                    test_a_login_with_no_token_is_refused);
    g_test_add_func("/matrix/login-rubbish", test_rubbish_is_refused);
    g_test_add_func("/matrix/rooms", test_joined_rooms_are_read);
    g_test_add_func("/matrix/rooms-empty",
                    test_an_account_in_no_rooms_is_not_an_error);
    g_test_add_func("/matrix/rooms-rejected",
                    test_a_rejected_room_list_is_an_error);
    g_test_add_func("/matrix/room-label", test_a_room_describes_itself);
    g_test_add_func("/matrix/send", test_a_notice_is_posted);

    return g_test_run();
}
