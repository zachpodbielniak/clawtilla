/*
 * test-connection.c - Saved ways of reaching a daemon
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A connections file is touched when somebody edits their connections and
 * at no other time, so a field that survives being written and does not
 * survive being read is found months later by the person whose remote
 * host stopped working.  The round trip is asserted here instead.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>

#include <string.h>

#include "clawt-test-util.h"

static GPtrArray *
empty_list(void)
{
    return g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_connection_free);
}

static void
test_a_remote_survives_the_round_trip(void)
{
    g_autoptr(GPtrArray) list = empty_list();
    g_autofree gchar *text = NULL;
    g_autoptr(GPtrArray) back = NULL;
    g_autoptr(GError) error = NULL;
    ClawtConnection *connection;

    connection = clawt_connection_new_remote("workstation", "100.72.0.41",
                                              8792, "s3cret");
    clawt_connection_set_tls(connection, TRUE, TRUE);
    g_ptr_array_add(list, connection);

    text = clawt_connection_list_to_data(list);
    back = clawt_connection_list_parse(text, &error);

    g_assert_no_error(error);
    g_assert_cmpuint(back->len, ==, 1);

    connection = g_ptr_array_index(back, 0);
    g_assert_cmpstr(clawt_connection_get_name(connection), ==, "workstation");
    g_assert_cmpstr(clawt_connection_get_host(connection), ==, "100.72.0.41");
    g_assert_cmpuint(clawt_connection_get_port(connection), ==, 8792);
    g_assert_cmpstr(clawt_connection_get_token(connection), ==, "s3cret");
    g_assert_true(clawt_connection_get_tls(connection));
    g_assert_true(
        clawt_connection_get_accept_unknown_certificate(connection));
    g_assert_false(clawt_connection_is_local(connection));
}

/*
 * A token is generated from /dev/urandom and rendered base16 today, but
 * daemon.token_file lets a person supply their own -- and a passphrase
 * out of a password manager contains whatever it contains.  Single-quoted
 * YAML has exactly one escape, a doubled quote, which is why it is used;
 * this is the assertion that says so.
 */
static void
test_awkward_characters_survive_quoting(void)
{
    static const gchar *const nasty[] = {
        "tok'en",
        "back\\slash",
        "double\"quote",
        "hash # not a comment",
        "colon: space",
        "- leading dash",
        "{brace} [bracket]",
        "trailing space ",
        "100%",
        NULL
    };
    gsize i;

    for (i = 0; nasty[i] != NULL; i++) {
        g_autoptr(GPtrArray) list = empty_list();
        g_autofree gchar *text = NULL;
        g_autoptr(GPtrArray) back = NULL;
        g_autoptr(GError) error = NULL;

        g_ptr_array_add(list, clawt_connection_new_remote(
                                  nasty[i], "10.0.0.1", 8792, nasty[i]));

        text = clawt_connection_list_to_data(list);
        back = clawt_connection_list_parse(text, &error);

        g_assert_no_error(error);
        g_assert_cmpuint(back->len, ==, 1);
        g_assert_cmpstr(
            clawt_connection_get_token(g_ptr_array_index(back, 0)), ==,
            nasty[i]);
        g_assert_cmpstr(
            clawt_connection_get_name(g_ptr_array_index(back, 0)), ==,
            nasty[i]);
    }
}

static void
test_a_local_connection_has_no_host(void)
{
    g_autoptr(GPtrArray) list = empty_list();
    g_autofree gchar *text = NULL;
    g_autoptr(GPtrArray) back = NULL;
    ClawtConnection *connection;

    g_ptr_array_add(list, clawt_connection_new_local("Second daemon",
                                                      "/run/other.sock"));

    text = clawt_connection_list_to_data(list);
    back = clawt_connection_list_parse(text, NULL);

    g_assert_cmpuint(back->len, ==, 1);
    connection = g_ptr_array_index(back, 0);

    g_assert_true(clawt_connection_is_local(connection));
    g_assert_cmpstr(clawt_connection_get_socket_path(connection), ==,
                    "/run/other.sock");
}

/*
 * A profile with no usable port is dropped rather than given a default.
 *
 * Connecting to the wrong port is refused by the far end in a way that
 * reads as "no daemon is running there", which sends a person to check
 * the wrong machine.  An entry that never appears is at least a question
 * about this file.
 */
static void
test_a_profile_without_a_port_is_skipped(void)
{
    static const gchar *const bad =
        "connections:\n"
        "  -\n"
        "    name: 'no port'\n"
        "    host: '10.0.0.1'\n"
        "  -\n"
        "    name: 'zero'\n"
        "    host: '10.0.0.2'\n"
        "    port: 0\n"
        "  -\n"
        "    name: 'too big'\n"
        "    host: '10.0.0.3'\n"
        "    port: 70000\n"
        "  -\n"
        "    name: 'fine'\n"
        "    host: '10.0.0.4'\n"
        "    port: 8792\n";
    g_autoptr(GPtrArray) list = clawt_connection_list_parse(bad, NULL);

    g_assert_nonnull(list);
    g_assert_cmpuint(list->len, ==, 1);
    g_assert_cmpstr(clawt_connection_get_name(g_ptr_array_index(list, 0)),
                    ==, "fine");
}

static void
test_an_empty_file_is_an_empty_list(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GPtrArray) empty = clawt_connection_list_parse("", &error);
    g_autoptr(GPtrArray) commented =
        clawt_connection_list_parse("# nothing here\n", NULL);
    g_autoptr(GPtrArray) none =
        clawt_connection_list_parse("connections:\n  []\n", NULL);

    g_assert_no_error(error);
    g_assert_nonnull(empty);
    g_assert_cmpuint(empty->len, ==, 0);

    g_assert_nonnull(commented);
    g_assert_cmpuint(commented->len, ==, 0);

    g_assert_nonnull(none);
    g_assert_cmpuint(none->len, ==, 0);
}

/*
 * What to_data() writes must be what parse() reads, including when the
 * list is empty -- the empty case is the one a person reaches first, and
 * a file that cannot be read back is a file that loses the connection
 * they then add to it.
 */
static void
test_the_empty_rendering_parses(void)
{
    g_autoptr(GPtrArray) list = empty_list();
    g_autofree gchar *text = clawt_connection_list_to_data(list);
    g_autoptr(GError) error = NULL;
    g_autoptr(GPtrArray) back = clawt_connection_list_parse(text, &error);

    g_assert_no_error(error);
    g_assert_nonnull(back);
    g_assert_cmpuint(back->len, ==, 0);
}

/*
 * describe() ends up in a header bar subtitle and in `remote list`, both
 * of which get read over somebody's shoulder.
 */
static void
test_a_description_never_carries_the_token(void)
{
    g_autoptr(ClawtConnection) remote =
        clawt_connection_new_remote("box", "100.64.1.2", 8792,
                                    "TOKEN-SHOULD-NOT-APPEAR");
    g_autoptr(ClawtConnection) local =
        clawt_connection_new_local("Local", NULL);
    g_autofree gchar *remote_text = clawt_connection_describe(remote);
    g_autofree gchar *local_text = clawt_connection_describe(local);

    g_assert_null(strstr(remote_text, "TOKEN-SHOULD-NOT-APPEAR"));
    g_assert_nonnull(strstr(remote_text, "100.64.1.2"));
    g_assert_nonnull(strstr(remote_text, "8792"));

    g_assert_nonnull(local_text);
    g_assert_cmpstr(local_text, !=, "");
}

static void
test_find_matches_on_the_name(void)
{
    g_autoptr(GPtrArray) list = empty_list();

    g_ptr_array_add(list, clawt_connection_new_local("Local", NULL));
    g_ptr_array_add(list,
                    clawt_connection_new_remote("box", "10.0.0.1", 1, NULL));

    g_assert_nonnull(clawt_connection_list_find(list, "box"));
    g_assert_nonnull(clawt_connection_list_find(list, "Local"));
    g_assert_null(clawt_connection_list_find(list, "BOX"));
    g_assert_null(clawt_connection_list_find(list, "missing"));
    g_assert_null(clawt_connection_list_find(NULL, "box"));
}

/*
 * The file holds bearer tokens.  0600 is the whole of its protection, and
 * a mode that drifted would be invisible until somebody else read it.
 */
static void
test_the_file_is_written_private(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-conn-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(dir, "sub", "conn.yaml", NULL);
    g_autoptr(GPtrArray) list = empty_list();
    g_autoptr(GError) error = NULL;
    g_autoptr(GPtrArray) back = NULL;
    GStatBuf info;

    g_ptr_array_add(list, clawt_connection_new_remote("box", "10.0.0.1",
                                                       8792, "s3cret"));

    g_assert_true(clawt_connection_list_save(path, list, &error));
    g_assert_no_error(error);

    g_assert_cmpint(g_stat(path, &info), ==, 0);
    g_assert_cmpuint(info.st_mode & 0777, ==, 0600);

    /* And the directory it had to create along the way. */
    {
        g_autofree gchar *parent = g_path_get_dirname(path);
        GStatBuf parent_info;

        g_assert_cmpint(g_stat(parent, &parent_info), ==, 0);
        g_assert_cmpuint(parent_info.st_mode & 0777, ==, 0700);
    }

    back = clawt_connection_list_load(path, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(back->len, ==, 1);
    g_assert_cmpstr(clawt_connection_get_token(g_ptr_array_index(back, 0)),
                    ==, "s3cret");

    g_unlink(path);
}

/*
 * Never having saved a remote host is the ordinary state, not a fault --
 * so a missing file must not be an error the clients have to special-case
 * on every start.
 */
static void
test_a_missing_file_is_not_an_error(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-conn-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(dir, "absent.yaml", NULL);
    g_autoptr(GError) error = NULL;
    g_autoptr(GPtrArray) list = clawt_connection_list_load(path, &error);

    g_assert_no_error(error);
    g_assert_nonnull(list);
    g_assert_cmpuint(list->len, ==, 0);

    g_rmdir(dir);
}

/*
 * A client built from a profile has to be the one the profile describes.
 * This is the seam the GTK switcher and the CLI's --profile both go
 * through, and there is no other place it is checked.
 */
static void
test_a_client_is_built_from_the_profile(void)
{
    g_autoptr(ClawtConnection) local =
        clawt_connection_new_local("Local", "/run/clawt/test.sock");
    g_autoptr(ClawtConnection) remote =
        clawt_connection_new_remote("box", "100.64.1.2", 9999, "t");
    g_autoptr(ClawtClient) from_local =
        clawt_connection_create_client(local);
    g_autoptr(ClawtClient) from_remote =
        clawt_connection_create_client(remote);

    /*
     * Neither is connected, which is the point: building a client must
     * not reach the network, so a profile naming a machine that is
     * switched off still produces an object to report the failure.
     */
    g_assert_nonnull(from_local);
    g_assert_nonnull(from_remote);
    g_assert_false(clawt_client_is_connected(from_local));
    g_assert_false(clawt_client_is_connected(from_remote));
}

static void
test_a_copy_is_independent(void)
{
    g_autoptr(ClawtConnection) original =
        clawt_connection_new_remote("box", "10.0.0.1", 8792, "s3cret");
    g_autoptr(ClawtConnection) copy = clawt_connection_copy(original);

    clawt_connection_set_name(original, "renamed");

    g_assert_cmpstr(clawt_connection_get_name(copy), ==, "box");
    g_assert_cmpstr(clawt_connection_get_token(copy), ==, "s3cret");
    g_assert_cmpuint(clawt_connection_get_port(copy), ==, 8792);
}

/*
 * "Refused" and "unreachable" are different answers.
 *
 * One is a credential problem and the other is a network problem, and
 * telling somebody to check the wrong one costs a long time: a rotated
 * token and a sleeping host produce the same silence from a client that
 * does not distinguish them.
 */
static void
test_a_refusal_is_not_the_same_as_silence(void)
{
    g_autoptr(GError) refused = g_error_new_literal(
        CLAWT_ERROR, CLAWT_ERROR_AUTH, "that token was not accepted");
    g_autoptr(GError) unreachable = g_error_new_literal(
        G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED, "Connection refused");
    g_autoptr(GError) timed_out = g_error_new_literal(
        CLAWT_ERROR, CLAWT_ERROR_TIMEOUT, "the daemon did not answer");

    g_assert_cmpint(clawt_reachability_from_error(refused), ==,
                    CLAWT_REACH_REFUSED);

    /*
     * G_IO_ERROR_CONNECTION_REFUSED is a *network* refusal -- nothing is
     * listening -- and must not be read as a credential one just because
     * both are spelled "refused" in English.
     */
    g_assert_cmpint(clawt_reachability_from_error(unreachable), ==,
                    CLAWT_REACH_UNREACHABLE);
    g_assert_cmpint(clawt_reachability_from_error(timed_out), ==,
                    CLAWT_REACH_UNREACHABLE);

    /*
     * The positive control: no error at all is reachable. Without it the
     * three assertions above would pass in a build that never answered
     * REACHABLE, which is the one answer the menu exists to give.
     */
    g_assert_cmpint(clawt_reachability_from_error(NULL), ==,
                    CLAWT_REACH_REACHABLE);
}

/*
 * Each verdict has a word of its own, and no two share one.
 *
 * Written as a sweep rather than four assertions, because the failure
 * this guards against is a new verdict added with a copied string.
 */
static void
test_every_verdict_has_its_own_word(void)
{
    ClawtReachability all[] = {
        CLAWT_REACH_UNKNOWN, CLAWT_REACH_REACHABLE,
        CLAWT_REACH_REFUSED, CLAWT_REACH_UNREACHABLE
    };
    gsize i, j;

    for (i = 0; i < G_N_ELEMENTS(all); i++) {
        const gchar *word = clawt_reachability_word(all[i]);

        g_assert_nonnull(word);
        g_assert_cmpstr(word, !=, "");

        for (j = 0; j < i; j++)
            g_assert_cmpstr(word, !=, clawt_reachability_word(all[j]));
    }
}


int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/connection/refusal-is-not-silence",
                    test_a_refusal_is_not_the_same_as_silence);
    g_test_add_func("/connection/every-verdict-has-a-word",
                    test_every_verdict_has_its_own_word);
    g_test_add_func("/connection/round-trip",
                    test_a_remote_survives_the_round_trip);
    g_test_add_func("/connection/quoting",
                    test_awkward_characters_survive_quoting);
    g_test_add_func("/connection/local", test_a_local_connection_has_no_host);
    g_test_add_func("/connection/no-port",
                    test_a_profile_without_a_port_is_skipped);
    g_test_add_func("/connection/empty-file",
                    test_an_empty_file_is_an_empty_list);
    g_test_add_func("/connection/empty-round-trip",
                    test_the_empty_rendering_parses);
    g_test_add_func("/connection/describe-hides-the-token",
                    test_a_description_never_carries_the_token);
    g_test_add_func("/connection/find", test_find_matches_on_the_name);
    g_test_add_func("/connection/file-is-private",
                    test_the_file_is_written_private);
    g_test_add_func("/connection/missing-file",
                    test_a_missing_file_is_not_an_error);
    g_test_add_func("/connection/creates-a-client",
                    test_a_client_is_built_from_the_profile);
    g_test_add_func("/connection/copy", test_a_copy_is_independent);

    return g_test_run();
}
