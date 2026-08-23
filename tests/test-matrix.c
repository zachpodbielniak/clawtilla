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

    return g_test_run();
}
