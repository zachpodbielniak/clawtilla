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

#include <gio/gunixsocketaddress.h>
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

    /*
     * The tree.  The path is two levels down on purpose -- the assertion
     * above is that saving created the 0700 directory along the way --
     * so unlinking the file left that directory and the temporary one
     * holding it behind on every run.
     */
    clawt_test_remove_tree(dir);
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

    clawt_test_remove_tree(dir);
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



/* ── Which version the other end is ──────────────────────────────── */

/*
 * The comparison is on numbers, not on strings.
 *
 * "0.10.0" sorts *before* "0.9.0" under strcmp, which is the comparison
 * somebody reaches for and the one that is wrong exactly when it starts
 * to matter -- the first time a minor reaches double figures, which is
 * also the first time anybody has been running long enough to have two
 * different builds talking to each other.
 */
static void
test_versions_compare_as_numbers(void)
{
    g_assert_cmpint(clawt_version_compare_to_client(CLAWT_VERSION_STRING),
                    ==, CLAWT_VERSION_SAME);

    g_assert_cmpint(clawt_version_compare_to_client("999.0.0"), ==,
                    CLAWT_VERSION_DAEMON_NEWER);
    g_assert_cmpint(clawt_version_compare_to_client("0.0.0"), ==,
                    CLAWT_VERSION_DAEMON_OLDER);

    /*
     * The trap itself, stated against two versions rather than against
     * whatever this build happens to be.  A test that could only reach
     * it once the build had passed 0.10 would first fail in the release
     * where finding out is too late.
     */
    g_assert_cmpint(clawt_version_compare("0.10.0", "0.9.0"), ==,
                    CLAWT_VERSION_DAEMON_NEWER);
    g_assert_cmpint(clawt_version_compare("0.9.0", "0.10.0"), ==,
                    CLAWT_VERSION_DAEMON_OLDER);

    /* Which is the opposite of what a string comparison says. */
    g_assert_cmpint(strcmp("0.10.0", "0.9.0"), <, 0);

    /* And every component counts, not only the first that differs. */
    g_assert_cmpint(clawt_version_compare("1.2.3", "1.2.3"), ==,
                    CLAWT_VERSION_SAME);
    g_assert_cmpint(clawt_version_compare("1.2.4", "1.2.3"), ==,
                    CLAWT_VERSION_DAEMON_NEWER);
    g_assert_cmpint(clawt_version_compare("2.0.0", "1.99.99"), ==,
                    CLAWT_VERSION_DAEMON_NEWER);
    g_assert_cmpint(clawt_version_compare("1.99.99", "2.0.0"), ==,
                    CLAWT_VERSION_DAEMON_OLDER);
}

/*
 * Anything we cannot place is UNKNOWN, and UNKNOWN says nothing.
 *
 * A daemon that predates the field, or one built from a dirty tree, is
 * not a daemon we have established anything about -- and a client that
 * announced "this daemon's version is unreadable" on every connect would
 * be crying wolf at the case the check exists to keep quiet.
 */
static void
test_an_unreadable_version_is_not_a_verdict(void)
{
    const gchar *unreadable[] = { NULL, "", "nightly", "0", "0.1.0-dirty",
                                  "0.x.0", "..", "0.1.0.0.0" };
    guint i;

    for (i = 0; i < G_N_ELEMENTS(unreadable); i++) {
        g_autofree gchar *text = NULL;

        g_assert_cmpint(clawt_version_compare_to_client(unreadable[i]), ==,
                        CLAWT_VERSION_UNKNOWN);

        text = clawt_version_mismatch_text(unreadable[i]);
        g_assert_null(text);
    }

    /*
     * A missing micro is a version, not a failure: "0.2" is something
     * somebody may genuinely report, and calling it unreadable would
     * turn a real mismatch into a shrug.
     */
    g_assert_cmpint(clawt_version_compare_to_client("999.0"), ==,
                    CLAWT_VERSION_DAEMON_NEWER);
}

/*
 * The sentence names both versions and which way round it is.
 *
 * Which way round is the whole point: a newer daemon may offer something
 * this client cannot ask for, an older one may refuse something it will
 * send, and "version mismatch" leaves somebody guessing which of their
 * two machines to update.
 */
static void
test_the_mismatch_says_which_way_round(void)
{
    g_autofree gchar *newer = clawt_version_mismatch_text("999.0.0");
    g_autofree gchar *older = clawt_version_mismatch_text("0.0.0");
    g_autofree gchar *same =
        clawt_version_mismatch_text(CLAWT_VERSION_STRING);

    g_assert_nonnull(newer);
    g_assert_nonnull(strstr(newer, "999.0.0"));
    g_assert_nonnull(strstr(newer, CLAWT_VERSION_STRING));
    g_assert_nonnull(strstr(newer, "update the client"));

    g_assert_nonnull(older);
    g_assert_nonnull(strstr(older, "0.0.0"));
    g_assert_nonnull(strstr(older, CLAWT_VERSION_STRING));
    g_assert_nonnull(strstr(older, "update the daemon"));

    /* And nothing at all when they agree. */
    g_assert_null(same);
}

/* ── A reconnect that could not resume ───────────────────────────── */

/*
 * A daemon that answers `resumed: false` on the second connection.
 *
 * Driven by a fake rather than by a real daemon on purpose.  The
 * condition is a replay buffer that has dropped events past a client's
 * cursor, and reaching that against a real bus means publishing more
 * than a thousand events to provoke one boolean -- a slow test of the
 * wrong thing.  What is under test is what the *client* does when told,
 * and that is one flag in one reply.
 *
 * On its own thread with its own context, so the blocking reads here
 * never interleave with the client's loop, which the test thread turns.
 */
typedef struct {
    gchar        *path;
    GMainContext *context;
    GMainLoop    *loop;
    GThread      *thread;
    GSocketService *service;
    gint          connections;   /* accepted so far; atomic */

    /*
     * A daemon that does not hang up after the first subscribe.
     *
     * The resync test needs the connection dropped, because dropping it
     * is what starts the retry.  The tests below need the opposite: the
     * retry has already started and what is being checked is what the
     * client does once it gets through.
     */
    gboolean      stay;
    gint          subscribes;    /* atomic */

    /*
     * A daemon that answers without a payload at all.
     *
     * Eleven of clawtillad's own handlers do -- agent.start and
     * agent.restart among them -- so this is not a hypothetical shape.
     */
    gboolean      no_payload;
} FakeDaemon;

static void
send_line(GOutputStream *out, JsonNode *frame)
{
    g_autofree gchar *line = clawt_ipc_frame_to_line(frame);
    g_autofree gchar *wire = g_strconcat(line, "\n", NULL);

    g_output_stream_write_all(out, wire, strlen(wire), NULL, NULL, NULL);
}

static gboolean
on_fake_incoming(GSocketService *service, GSocketConnection *connection,
                 GObject *source, gpointer user_data)
{
    FakeDaemon *fake = user_data;
    g_autoptr(GDataInputStream) in = g_data_input_stream_new(
        g_io_stream_get_input_stream(G_IO_STREAM(connection)));
    GOutputStream *out =
        g_io_stream_get_output_stream(G_IO_STREAM(connection));
    gint which;

    (void)service;
    (void)source;

    which = g_atomic_int_add(&fake->connections, 1) + 1;
    g_data_input_stream_set_newline_type(in, G_DATA_STREAM_NEWLINE_TYPE_ANY);

    for (;;) {
        g_autofree gchar *line = NULL;
        g_autoptr(JsonNode) request = NULL;
        g_autoptr(JsonBuilder) builder = NULL;
        g_autoptr(JsonNode) reply = NULL;
        const gchar *kind;

        line = g_data_input_stream_read_line(in, NULL, NULL, NULL);

        if (line == NULL)
            break;

        request = clawt_ipc_frame_from_line(line, NULL);

        if (request == NULL)
            break;

        kind = clawt_ipc_frame_get_kind(request);

        if (fake->no_payload) {
            g_autoptr(JsonNode) bare = clawt_ipc_response_new(request, NULL);

            send_line(out, bare);
            continue;
        }

        builder = json_builder_new();
        json_builder_begin_object(builder);

        if (g_strcmp0(kind, "control.subscribe") == 0) {
            g_atomic_int_inc(&fake->subscribes);
            json_builder_set_member_name(builder, "cursor");
            json_builder_add_int_value(builder, 7);
            json_builder_set_member_name(builder, "resumed");

            /*
             * The first connection resumes cleanly.  That is the control:
             * without it a client that emitted ::resync on every
             * subscribe would pass.
             */
            json_builder_add_boolean_value(builder, which == 1);
        } else {
            json_builder_set_member_name(builder, "version");
            json_builder_add_string_value(builder, CLAWT_VERSION_STRING);
        }

        json_builder_end_object(builder);
        reply = clawt_ipc_response_new(request, json_builder_get_root(builder));
        send_line(out, reply);

        /*
         * One event, so the client's cursor is past zero -- a cursor of 0
         * is always resumable, so without this the second subscribe would
         * ask for something any daemon could honour.
         */
        if (g_strcmp0(kind, "control.subscribe") == 0 && which == 1 &&
            !fake->stay) {
            g_autoptr(ClawtEvent) event = clawt_event_new("daemon.started",
                                                          "fake");
            g_autoptr(JsonNode) frame = NULL;

            clawt_event_set_cursor(event, 7);
            frame = clawt_ipc_event_new(event);
            send_line(out, frame);

            /* And then it goes away, which is what starts the retry. */
            g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
            break;
        }
    }

    return TRUE;
}

static gpointer
run_fake_daemon(gpointer data)
{
    FakeDaemon *fake = data;

    g_main_context_push_thread_default(fake->context);
    g_socket_service_start(fake->service);
    g_main_loop_run(fake->loop);
    g_socket_service_stop(fake->service);
    g_main_context_pop_thread_default(fake->context);

    return NULL;
}

static FakeDaemon *
fake_daemon_start_full(const gchar *path, gboolean stay, gboolean no_payload)
{
    FakeDaemon *fake = g_new0(FakeDaemon, 1);
    g_autoptr(GSocketAddress) address = NULL;
    g_autoptr(GError) error = NULL;

    fake->path = g_strdup(path);
    fake->stay = stay;
    fake->no_payload = no_payload;
    fake->context = g_main_context_new();
    fake->loop = g_main_loop_new(fake->context, FALSE);

    fake->service = g_socket_service_new();

    /*
     * Stopped before the address is added, and started again on the
     * fake's own thread.
     *
     * g_socket_service_new() returns an *active* service, and adding an
     * address to an active one attaches its accept source to whatever is
     * thread-default right now -- which here is the test thread's, not
     * the loop this fake actually runs.  The service then listened on a
     * context nobody was iterating and the test hung waiting for a
     * connection that was never accepted.
     */
    g_socket_service_stop(fake->service);

    address = g_unix_socket_address_new(fake->path);
    g_assert_true(g_socket_listener_add_address(
        G_SOCKET_LISTENER(fake->service), address, G_SOCKET_TYPE_STREAM,
        G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL, &error));
    g_assert_no_error(error);

    g_signal_connect(fake->service, "incoming",
                     G_CALLBACK(on_fake_incoming), fake);

    fake->thread = g_thread_new("fake-daemon", run_fake_daemon, fake);

    return fake;
}

static FakeDaemon *
fake_daemon_start_at(const gchar *path, gboolean stay)
{
    return fake_daemon_start_full(path, stay, FALSE);
}

static FakeDaemon *
fake_daemon_start(const gchar *dir)
{
    g_autofree gchar *path = g_build_filename(dir, "fake.sock", NULL);

    return fake_daemon_start_at(path, FALSE);
}

/* Answers every request with `ok` and nothing else. */
static FakeDaemon *
fake_daemon_start_silent(const gchar *dir)
{
    g_autofree gchar *path = g_build_filename(dir, "silent.sock", NULL);

    return fake_daemon_start_full(path, FALSE, TRUE);
}

static void
fake_daemon_stop(FakeDaemon *fake)
{
    g_main_loop_quit(fake->loop);
    g_thread_join(fake->thread);

    /*
     * Joined before the listener goes.  A thread parked in accept() with
     * a finalised listener is a G_IS_SOCKET assertion printed after the
     * last test has already reported ok, which reads as a suite-level
     * fault rather than as one test's teardown.
     */
    g_clear_object(&fake->service);
    g_main_loop_unref(fake->loop);
    g_main_context_unref(fake->context);
    g_unlink(fake->path);
    g_free(fake->path);
    g_free(fake);
}

typedef struct {
    guint    resync;
    guint    connected;
    guint    disconnected;
    gboolean retry_arranged;   /* as seen from inside ::disconnected */
} ResyncWatch;

static void
note_resync(ClawtClient *client, gpointer data)
{
    ResyncWatch *watch = data;

    (void)client;
    watch->resync++;
}

static void
note_reconnected(ClawtClient *client, gpointer data)
{
    ResyncWatch *watch = data;

    (void)client;
    watch->connected++;
}

/*
 * Records whether the retry was already arranged at the moment the
 * signal fired.
 *
 * A subscriber's whole reason to exist is to draw the state, and
 * handle_disconnect() used to emit *before* scheduling -- so both
 * clients were told the connection had gone and then found
 * clawt_client_is_reconnecting() answering FALSE, and drew nothing.
 * Found by killing a daemon under the real GTK client and reading a
 * probe; no test could see it, because a test that samples the state in
 * a polling loop always samples it after the handler has returned.
 */
static void
note_disconnected(ClawtClient *client, gpointer data)
{
    ResyncWatch *watch = data;

    watch->disconnected++;
    watch->retry_arranged = clawt_client_is_reconnecting(client);
}

/*
 * A client that loses its daemon says so, and gets it back on its *own*
 * main context.
 *
 * Two defects in one path.  Nothing in either graphical client had ever
 * connected to ::disconnected, so a daemon that went away mid-session
 * looked exactly like a fleet that had gone quiet: the agent list was
 * the last one received, no event arrived, and nothing said why.  And
 * the retry timer went in through g_timeout_add_seconds(), which
 * attaches to the *global default* context -- not the one this client's
 * reader is on, and dispatching a source pushes nothing, so a
 * handle_disconnect() reached from that reader had no thread-default to
 * inherit either.  An embedded host running its own loop therefore had a
 * client that lost its daemon and never once tried to get it back.
 *
 * The default context is never iterated here.  A build whose timer or
 * whose reader lands on it makes no progress at all -- which is exactly
 * what an embedded host sees, and what no test running on the default
 * context could ever have shown.
 *
 * The fake hangs up on its own after answering the first subscribe,
 * which is what a daemon exiting looks like from this end.
 */
static void
test_a_client_reconnects_on_its_own_context(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-recon-XXXXXX", NULL);
    g_autoptr(GMainContext) context = g_main_context_new();
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(GError) error = NULL;
    FakeDaemon *fake = fake_daemon_start(dir);
    ResyncWatch watch = { 0 };
    GLogLevelFlags fatal;
    gint64 deadline;
    gboolean saw_reconnecting = FALSE;

    client = clawt_client_new(fake->path);

    g_main_context_push_thread_default(context);
    g_assert_true(clawt_client_connect(client, &error));
    g_main_context_pop_thread_default(context);
    g_assert_no_error(error);

    clawt_client_set_auto_reconnect(client, TRUE);
    g_signal_connect(client, "connected", G_CALLBACK(note_reconnected),
                     &watch);
    g_signal_connect(client, "disconnected", G_CALLBACK(note_disconnected),
                     &watch);

    g_assert_true(clawt_client_is_connected(client));

    /*
     * Not reconnecting, which is a different thing from not being
     * connected: a client nobody has connected yet is also not
     * connected, and that is not a state worth drawing.
     */
    g_assert_false(clawt_client_is_reconnecting(client));

    g_assert_true(clawt_client_subscribe(client, 0, NULL, &error));
    g_assert_no_error(error);

    fatal = g_log_set_always_fatal(0);
    deadline = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;

    while (watch.connected == 0 && g_get_monotonic_time() < deadline) {
        g_main_context_iteration(context, FALSE);

        /*
         * Sampled inside the loop.  The state is transient by design --
         * it ends the moment the retry succeeds -- so a check after the
         * wait would find it already over and pass against a build that
         * never entered it.
         */
        if (clawt_client_is_reconnecting(client))
            saw_reconnecting = TRUE;

        g_usleep(2 * 1000);
    }

    g_log_set_always_fatal(fatal);

    g_assert_cmpuint(watch.disconnected, ==, 1);

    /*
     * And it was already true *inside* the handler, not merely at some
     * point afterwards.  That ordering is the difference between a
     * client that draws the state and one that is told about it and has
     * nothing to say.
     */
    g_assert_true(watch.retry_arranged);
    g_assert_true(saw_reconnecting);
    g_assert_cmpuint(watch.connected, ==, 1);
    g_assert_true(clawt_client_is_connected(client));
    g_assert_false(clawt_client_is_reconnecting(client));

    clawt_client_disconnect(client);
    fake_daemon_stop(fake);
    clawt_test_remove_tree(dir);
}

/* Breaks a nested wait that is never going to be answered. */
static gboolean
give_up_waiting(gpointer user_data)
{
    ClawtClient *client = user_data;

    clawt_client_set_auto_reconnect(client, FALSE);
    clawt_client_disconnect(client);

    return G_SOURCE_REMOVE;
}

/*
 * A reconnect that could not resume tells the application.
 *
 * The daemon replays from a bounded buffer, so an outage longer than
 * that buffer comes back `resumed: false`, which means the client has a
 * hole in its view.  It is precisely the shape a reconnect after a long
 * outage takes, which is when it matters most -- and for a long time the
 * only thing that happened was a warning in the journal: the window went
 * on showing state from before the outage indefinitely, with nothing to
 * say so.  Both graphical clients answer ::resync by re-reading.
 */
static void
test_a_reconnect_that_cannot_resume_says_so(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-resync-XXXXXX", NULL);
    g_autoptr(GMainContext) context = g_main_context_new();
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(GError) error = NULL;
    FakeDaemon *fake = fake_daemon_start(dir);
    ResyncWatch watch = { 0 };
    GLogLevelFlags fatal;
    GSource *watchdog;
    gint64 deadline;

    client = clawt_client_new(fake->path);

    g_main_context_push_thread_default(context);
    g_assert_true(clawt_client_connect(client, &error));
    g_main_context_pop_thread_default(context);
    g_assert_no_error(error);

    clawt_client_set_auto_reconnect(client, TRUE);
    g_signal_connect(client, "resync", G_CALLBACK(note_resync), &watch);
    g_signal_connect(client, "connected", G_CALLBACK(note_reconnected),
                     &watch);

    {
        gboolean resumed = FALSE;

        g_assert_true(clawt_client_subscribe(client, 0, &resumed, &error));
        g_assert_no_error(error);
        g_assert_true(resumed);
    }

    /*
     * The failure to resume is warned about on purpose, so GTest is told
     * not to abort on it.  g_test_expect_message() is the wrong tool: it
     * makes every *other* message fatal in turn.
     */
    fatal = g_log_set_always_fatal(0);

    /*
     * A watchdog, because this test can otherwise *hang* rather than
     * fail -- and a hang is worse: it is indistinguishable from a slow
     * machine or a loop somewhere else, and it stops every test after
     * it.
     *
     * The reconnect happens from a timeout callback on this context, and
     * the hello inside it waits by iterating the same context.  So if
     * the reply never arrives -- which is precisely what a reader armed
     * on the wrong context produces -- the nested wait runs for the full
     * two-minute request timeout and the loop below never gets another
     * turn to notice its own deadline.  Disconnecting from here breaks
     * that inner wait, and the assertions then fail with the reason.
     */
    watchdog = g_timeout_source_new_seconds(5);
    g_source_set_callback(watchdog, give_up_waiting, client, NULL);
    g_source_attach(watchdog, context);

    deadline = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;

    while (watch.resync == 0 && g_get_monotonic_time() < deadline) {
        g_main_context_iteration(context, FALSE);
        g_usleep(2 * 1000);
    }

    g_source_destroy(watchdog);
    g_source_unref(watchdog);

    g_log_set_always_fatal(fatal);

    g_assert_cmpuint(watch.connected, ==, 1);
    g_assert_cmpuint(watch.resync, ==, 1);

    clawt_client_disconnect(client);
    fake_daemon_stop(fake);
    clawt_test_remove_tree(dir);
}


/*
 * Turning auto-reconnect off stops the retries, including from inside an
 * attempt.
 *
 * A connect blocks for as long as the far end takes -- up to the whole
 * request timeout -- and the failure path rescheduled unconditionally,
 * so a caller that said stop during that window was ignored and the loop
 * ran for ever, each turn holding its context for another two minutes.
 * `set_auto_reconnect(FALSE)` is how a caller says stop; it has to work
 * from inside an attempt as well as before one.
 */
static void
test_auto_reconnect_can_be_turned_off_mid_flight(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-retry-XXXXXX", NULL);
    g_autoptr(GMainContext) context = g_main_context_new();
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(GError) error = NULL;
    FakeDaemon *fake = fake_daemon_start(dir);
    gint64 deadline;

    client = clawt_client_new(fake->path);

    g_main_context_push_thread_default(context);
    g_assert_true(clawt_client_connect(client, &error));
    g_main_context_pop_thread_default(context);
    g_assert_no_error(error);

    clawt_client_set_auto_reconnect(client, TRUE);

    /* The fake closes this connection once it has answered subscribe. */
    g_assert_true(clawt_client_subscribe(client, 0, NULL, &error));
    g_assert_no_error(error);

    /*
     * And then it is gone entirely, so every retry fails at once rather
     * than blocking -- which is what makes this test quick and is also
     * the path the reschedule lives on.
     */
    fake_daemon_stop(fake);

    deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;

    while (!clawt_client_is_reconnecting(client) &&
           g_get_monotonic_time() < deadline) {
        g_main_context_iteration(context, FALSE);
        g_usleep(2 * 1000);
    }

    g_assert_true(clawt_client_is_reconnecting(client));

    clawt_client_set_auto_reconnect(client, FALSE);

    /*
     * Long enough for the scheduled attempt to fire, fail, and decline
     * to schedule another.  Without the check it reschedules for ever
     * and this is still TRUE.
     */
    deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;

    while (clawt_client_is_reconnecting(client) &&
           g_get_monotonic_time() < deadline) {
        g_main_context_iteration(context, FALSE);
        g_usleep(2 * 1000);
    }

    g_assert_false(clawt_client_is_reconnecting(client));

    clawt_client_disconnect(client);
    clawt_test_remove_tree(dir);
}


/* ── What a client says about its own connection ─────────────────── */

/*
 * The three states are told apart by whether the client ever got there,
 * and by nothing else.
 *
 * A retry is scheduled in both the never and the lost case, so it cannot
 * be the discriminator -- which is exactly the mistake the banner made
 * for as long as it had two states instead of three.
 */
static void
test_never_and_lost_differ_only_in_history(void)
{
    g_autoptr(ClawtClient) client = clawt_client_new("/nonexistent/x.sock");

    g_assert_cmpint(clawt_daemon_link_state(client, FALSE), ==,
                    CLAWT_DAEMON_LINK_NEVER);
    g_assert_cmpint(clawt_daemon_link_state(client, TRUE), ==,
                    CLAWT_DAEMON_LINK_LOST);

    /* And no client at all is not a connection anybody has lost. */
    g_assert_cmpint(clawt_daemon_link_state(NULL, FALSE), ==,
                    CLAWT_DAEMON_LINK_NEVER);
}

/*
 * A local daemon is told to be started; a remote one is not.
 *
 * The remedies are different and only one of them is on this machine, so
 * telling somebody to run clawtillad for a workstation they cannot reach
 * sends them to start a daemon that gets them no closer.
 */
static void
test_the_advice_matches_where_the_daemon_is(void)
{
    g_autoptr(ClawtConnection) local =
        clawt_connection_new_local("Local", NULL);
    g_autoptr(ClawtConnection) remote =
        clawt_connection_new_remote("workstation", "100.72.0.41", 8792,
                                    "s3cret");
    g_autofree gchar *here = NULL;
    g_autofree gchar *there = NULL;

    here = clawt_connection_notice_text(CLAWT_DAEMON_LINK_NEVER, local, NULL);
    there = clawt_connection_notice_text(CLAWT_DAEMON_LINK_NEVER, remote,
                                         NULL);

    g_assert_nonnull(here);
    g_assert_nonnull(there);

    g_assert_nonnull(strstr(here, "clawtillad"));
    g_assert_null(strstr(there, "clawtillad"));

    /* Both offer the way out, which is the whole point of the sentence. */
    g_assert_nonnull(strstr(here, "connection"));
    g_assert_nonnull(strstr(there, "connection"));

    /* The remote one says which machine, since a name alone may not. */
    g_assert_nonnull(strstr(there, "100.72.0.41"));

    /*
     * And never the token.  This sentence is drawn in a banner, put in a
     * served page and read out of a log; clawt_connection_describe() is
     * the one that hides it and this has to be built on that one.
     */
    g_assert_null(strstr(there, "s3cret"));
}

/*
 * "Lost" is a claim about history, and it is wrong for a window that has
 * never had anything on it.
 */
static void
test_a_connection_never_made_was_not_lost(void)
{
    g_autoptr(ClawtConnection) local =
        clawt_connection_new_local("Local", NULL);
    g_autofree gchar *never = NULL;
    g_autofree gchar *lost = NULL;

    never = clawt_connection_notice_text(CLAWT_DAEMON_LINK_NEVER, local, NULL);
    lost = clawt_connection_notice_text(CLAWT_DAEMON_LINK_LOST, local, NULL);

    g_assert_nonnull(never);
    g_assert_nonnull(lost);
    g_assert_cmpstr(never, !=, lost);

    g_assert_null(strstr(never, "Lost"));
    g_assert_nonnull(strstr(lost, "Lost"));

    /*
     * The lost sentence says what is on screen is stale.  The never one
     * must not: there is nothing on screen to be stale.
     */
    g_assert_nonnull(strstr(lost, "before it went"));
    g_assert_null(strstr(never, "before it went"));
}

/*
 * A connection that is down outranks a version mismatch.
 *
 * While it is down the version is whatever it was before it went, and
 * telling somebody to update a daemon they cannot reach is advice about
 * the wrong problem.
 */
static void
test_a_broken_connection_outranks_a_version(void)
{
    g_autoptr(ClawtConnection) local =
        clawt_connection_new_local("Local", NULL);
    g_autofree gchar *never = NULL;
    g_autofree gchar *lost = NULL;
    g_autofree gchar *up = NULL;

    /*
     * A version far enough from this build that
     * clawt_version_mismatch_text() definitely has something to say --
     * checked, so the test cannot pass by there being no mismatch.
     */
    up = clawt_connection_notice_text(CLAWT_DAEMON_LINK_UP, local, "99.0.0");
    g_assert_nonnull(up);
    g_assert_nonnull(strstr(up, "99.0.0"));

    never = clawt_connection_notice_text(CLAWT_DAEMON_LINK_NEVER, local,
                                         "99.0.0");
    lost = clawt_connection_notice_text(CLAWT_DAEMON_LINK_LOST, local,
                                        "99.0.0");

    g_assert_null(strstr(never, "99.0.0"));
    g_assert_null(strstr(lost, "99.0.0"));
}

/* A connection that is up and agrees about the version says nothing. */
static void
test_a_healthy_connection_says_nothing(void)
{
    g_autoptr(ClawtConnection) local =
        clawt_connection_new_local("Local", NULL);
    g_autofree gchar *quiet = NULL;

    quiet = clawt_connection_notice_text(CLAWT_DAEMON_LINK_UP, local,
                                         CLAWT_VERSION_STRING);
    g_assert_null(quiet);

    /* An unknown version is not a complaint either. */
    quiet = clawt_connection_notice_text(CLAWT_DAEMON_LINK_UP, local, NULL);
    g_assert_null(quiet);
}

/*
 * A notice with no connection at all still reads as a sentence.
 *
 * The GTK window builds its banner before it has been given a profile in
 * at least one order of construction, and a "%s" printed as "(null)"
 * would be the first thing somebody saw.
 */
static void
test_a_notice_without_a_connection_still_reads(void)
{
    g_autofree gchar *never =
        clawt_connection_notice_text(CLAWT_DAEMON_LINK_NEVER, NULL, NULL);
    g_autofree gchar *lost =
        clawt_connection_notice_text(CLAWT_DAEMON_LINK_LOST, NULL, NULL);

    g_assert_nonnull(never);
    g_assert_nonnull(lost);
    g_assert_null(strstr(never, "(null)"));
    g_assert_null(strstr(lost, "(null)"));
}


/* ── A client that came up before its daemon ─────────────────────── */

/*
 * Iterates @context until @done or the deadline, whichever first.
 *
 * Against wall time rather than a fixed number of turns: the retry is a
 * one-second timer, so a count of iterations burns through long before
 * it could have fired and would report a failure about the clock.
 */
static gboolean
pump_until(GMainContext *context, gboolean (*done)(gpointer), gpointer data,
           guint seconds)
{
    gint64 deadline = g_get_monotonic_time() +
                      ((gint64)seconds * G_USEC_PER_SEC);

    while (!done(data) && g_get_monotonic_time() < deadline)
        g_main_context_iteration(context, FALSE);

    return done(data);
}

static gboolean
client_is_up(gpointer data)
{
    return clawt_client_is_connected(data);
}

/*
 * The bug this whole path exists for.
 *
 * A desktop client launched from a menu before its daemon is running had
 * no way back: the retry loop is armed by a connection *going away*, so
 * a first connect that failed left the client inert for ever and the
 * only remedy was to close the application and open it again.
 *
 * And the second half, which is the one that looks like it works: the
 * client reconnects, and receives nothing, because the subscription it
 * was asked for while the socket was down was never recorded.  A live
 * connection showing an empty fleet with no event ever arriving is worse
 * than no connection at all, since nothing says why.
 */
static void
test_a_client_that_never_connected_still_gets_there(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-never-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(dir, "later.sock", NULL);
    g_autoptr(GMainContext) context = g_main_context_new();
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(GError) error = NULL;
    FakeDaemon *fake;

    g_main_context_push_thread_default(context);
    client = clawt_client_new(path);
    clawt_client_set_auto_reconnect(client, TRUE);

    /* Nothing is listening there yet. */
    g_assert_false(clawt_client_connect(client, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);

    /*
     * Connect does not arm the retry by itself, and must not: the same
     * function is how the connection menu tries a machine somebody just
     * typed, where a failure has to be reported once rather than
     * retried behind them for ever.
     */
    g_assert_false(clawt_client_is_reconnecting(client));

    clawt_client_start_reconnecting(client);
    g_assert_true(clawt_client_is_reconnecting(client));

    /* Asked for while there was nothing to ask.  This fails, and counts. */
    g_assert_false(clawt_client_subscribe(client, 0, NULL, &error));
    g_clear_error(&error);

    fake = fake_daemon_start_at(path, TRUE);

    g_assert_true(pump_until(context, client_is_up, client, 20));

    /*
     * The subscription is the assertion that matters.  Merely being
     * connected is what the broken version also managed.
     */
    g_assert_cmpint(g_atomic_int_get(&fake->subscribes), >=, 1);

    clawt_client_disconnect(client);
    fake_daemon_stop(fake);
    g_main_context_pop_thread_default(context);
    clawt_test_remove_tree(dir);
}

/*
 * A caller with one thing to do gets its failure reported, not retried.
 *
 * The CLI runs a command and exits; a retry loop armed behind it would
 * hold its context for the whole backoff over a daemon that is simply
 * not running.
 */
static void
test_start_reconnecting_needs_permission(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-noretry-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(dir, "absent.sock", NULL);
    g_autoptr(GMainContext) context = g_main_context_new();
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(GError) error = NULL;

    g_main_context_push_thread_default(context);
    client = clawt_client_new(path);

    /* Auto-reconnect is off by default, and that is the answer here. */
    g_assert_false(clawt_client_connect(client, &error));
    g_clear_error(&error);

    clawt_client_start_reconnecting(client);
    g_assert_false(clawt_client_is_reconnecting(client));

    g_main_context_pop_thread_default(context);
    clawt_test_remove_tree(dir);
}

/*
 * Asked twice, armed once.
 *
 * Two timers on one client is not a cosmetic problem: the second one
 * overwrites the first's pointer without releasing the source, so the
 * first is leaked *and still attached*.  Both fire, and the extra ones
 * find the socket already up -- so the duplicate does not show as a
 * second connection, only as a second subscribe.  That is what is
 * counted here; an assertion on connections passes either way and
 * proves nothing, which is what the first draft of this test did.
 */
static void
test_start_reconnecting_does_not_stack(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-twice-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(dir, "twice.sock", NULL);
    g_autoptr(GMainContext) context = g_main_context_new();
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(GError) error = NULL;
    FakeDaemon *fake;

    g_main_context_push_thread_default(context);
    client = clawt_client_new(path);
    clawt_client_set_auto_reconnect(client, TRUE);

    g_assert_false(clawt_client_connect(client, &error));
    g_clear_error(&error);

    clawt_client_start_reconnecting(client);
    clawt_client_start_reconnecting(client);
    clawt_client_start_reconnecting(client);

    /* Wanted, so a duplicate timer has something to do twice. */
    g_assert_false(clawt_client_subscribe(client, 0, NULL, &error));
    g_clear_error(&error);

    fake = fake_daemon_start_at(path, TRUE);
    g_assert_true(pump_until(context, client_is_up, client, 20));

    /*
     * Given a moment for a second timer to fire if there were one.  The
     * base delay is a second, so two more seconds of turning the loop is
     * time for any duplicate to have shown itself.
     */
    {
        gint64 until = g_get_monotonic_time() + (2 * G_USEC_PER_SEC);

        while (g_get_monotonic_time() < until)
            g_main_context_iteration(context, FALSE);
    }

    g_assert_cmpint(g_atomic_int_get(&fake->connections), ==, 1);
    g_assert_cmpint(g_atomic_int_get(&fake->subscribes), ==, 1);

    clawt_client_disconnect(client);
    fake_daemon_stop(fake);
    g_main_context_pop_thread_default(context);
    clawt_test_remove_tree(dir);
}

/* There is nothing to get back when the daemon is already there. */
static void
test_a_connected_client_is_not_retried(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-up-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(dir, "up.sock", NULL);
    g_autoptr(GMainContext) context = g_main_context_new();
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(GError) error = NULL;
    FakeDaemon *fake = fake_daemon_start_at(path, TRUE);

    g_main_context_push_thread_default(context);
    client = clawt_client_new(path);
    clawt_client_set_auto_reconnect(client, TRUE);

    g_assert_true(clawt_client_connect(client, &error));
    g_assert_no_error(error);

    clawt_client_start_reconnecting(client);
    g_assert_false(clawt_client_is_reconnecting(client));

    clawt_client_disconnect(client);
    fake_daemon_stop(fake);
    g_main_context_pop_thread_default(context);
    clawt_test_remove_tree(dir);
}

/*
 * A reply that carried no payload is an *empty object*, not a node that
 * claims to be one.
 *
 * json_node_new(JSON_NODE_OBJECT) answers JSON_NODE_HOLDS_OBJECT() with
 * TRUE and json_node_get_object() with NULL, so every reader that checked
 * the type and then took the pointer got NULL from something it had just
 * been told was an object.  In the GTK client that reader is
 * clawt_ipc_reply_refusal_text(), which runs on *every* reply -- so
 * pressing Start printed a json-glib CRITICAL and the agent started
 * anyway, which is the shape that gets ignored until somebody reads their
 * console.
 *
 * Asserted on the node rather than on any one reader, because eleven
 * daemon handlers answer this way and the readers are in three clients.
 */
static void
test_a_reply_with_no_payload_is_an_empty_object(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-bare-XXXXXX", NULL);
    g_autoptr(GMainContext) context = g_main_context_new();
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *path = g_build_filename(dir, "silent.sock", NULL);
    FakeDaemon *fake = fake_daemon_start_silent(dir);

    g_main_context_push_thread_default(context);
    client = clawt_client_new(path);

    g_assert_true(clawt_client_connect(client, &error));
    g_assert_no_error(error);

    reply = clawt_client_request(client, "agent.start", NULL, &error);

    g_assert_no_error(error);
    g_assert_nonnull(reply);
    g_assert_true(JSON_NODE_HOLDS_OBJECT(reply));

    /* The whole point: the type and the contents agree. */
    g_assert_nonnull(json_node_get_object(reply));
    g_assert_cmpuint(json_object_get_size(json_node_get_object(reply)),
                     ==, 0);

    /*
     * And the reader that fires on every reply is happy with it.  Under
     * GTest a critical is fatal, so this line is the regression itself
     * rather than a restatement of the assertions above.
     */
    g_assert_null(clawt_ipc_reply_refusal_text(reply, NULL));

    clawt_client_disconnect(client);
    fake_daemon_stop(fake);
    g_main_context_pop_thread_default(context);
    clawt_test_remove_tree(dir);
}

typedef struct {
    ClawtConnection       *connection;
    ClawtConnectionStatus *status;
} ProbeRun;

static gpointer
run_probe(gpointer data)
{
    ProbeRun *run = data;

    run->status = clawt_connection_probe(run->connection);

    return NULL;
}

static gboolean
note_dispatch(gpointer data)
{
    gboolean *dispatched = data;

    *dispatched = TRUE;

    return G_SOURCE_REMOVE;
}

/*
 * A probe runs on a context of its own, whichever thread called it.
 *
 * clawt_connection_probe() blocks, so both graphical clients run it off
 * their main thread -- the GTK client from a GTask worker, the web client
 * from a request handler.  A ClawtClient settles on the thread-default
 * context when it connects, and a fresh worker thread has none, so the
 * probe used to settle on the *global* default: the context the
 * application's own loop lives on.  It then called
 * g_main_context_iteration() on it from the worker, which acquires that
 * context and dispatches its sources -- GTK's among them -- on a thread
 * that must never touch them.  On the GTK client that showed up first as
 * a pair of GLib criticals from g_main_context_push_thread_default(),
 * because the main thread already held the context.
 *
 * The assertion is deliberately not "no critical fired": the criticals
 * only appear when the caller's loop happens to be running, and the
 * dangerous case is the one where it is idle and the acquire *succeeds*.
 * So this arms a source on the default context and asserts the probe did
 * not dispatch it -- then dispatches it here, which is the control: a
 * test that armed nothing would pass either way.
 */
static void
test_a_probe_leaves_the_callers_context_alone(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-probe-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(dir, "silent.sock", NULL);
    g_autoptr(ClawtConnection) connection = NULL;
    g_autoptr(GSource) source = g_idle_source_new();
    FakeDaemon *fake = fake_daemon_start_silent(dir);
    gboolean dispatched = FALSE;
    ProbeRun run;
    GThread *thread;

    g_source_set_callback(source, note_dispatch, &dispatched, NULL);
    g_source_attach(source, g_main_context_default());

    connection = clawt_connection_new_local("silent", path);

    run.connection = connection;
    run.status = NULL;

    thread = g_thread_new("probe", run_probe, &run);
    g_thread_join(thread);

    g_assert_nonnull(run.status);

    /*
     * It really did talk to the fake.  Without this a probe that failed
     * before it reached the context at all would satisfy everything
     * below.
     */
    g_assert_cmpint(run.status->reach, ==, CLAWT_REACH_REACHABLE);

    g_assert_false(dispatched);

    /* The control. */
    while (g_main_context_iteration(g_main_context_default(), FALSE))
        ;

    g_assert_true(dispatched);

    g_source_destroy(source);
    clawt_connection_status_free(run.status);
    fake_daemon_stop(fake);
    clawt_test_remove_tree(dir);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/connection/never-connected-still-gets-there",
                    test_a_client_that_never_connected_still_gets_there);
    g_test_add_func("/connection/start-reconnecting-needs-permission",
                    test_start_reconnecting_needs_permission);
    g_test_add_func("/connection/start-reconnecting-does-not-stack",
                    test_start_reconnecting_does_not_stack);
    g_test_add_func("/connection/a-connected-client-is-not-retried",
                    test_a_connected_client_is_not_retried);
    g_test_add_func("/connection/bare-reply-is-an-empty-object",
                    test_a_reply_with_no_payload_is_an_empty_object);
    g_test_add_func("/connection/probe-leaves-the-callers-context-alone",
                    test_a_probe_leaves_the_callers_context_alone);
    g_test_add_func("/connection/never-and-lost-differ-only-in-history",
                    test_never_and_lost_differ_only_in_history);
    g_test_add_func("/connection/advice-matches-where-the-daemon-is",
                    test_the_advice_matches_where_the_daemon_is);
    g_test_add_func("/connection/never-made-was-not-lost",
                    test_a_connection_never_made_was_not_lost);
    g_test_add_func("/connection/broken-outranks-a-version",
                    test_a_broken_connection_outranks_a_version);
    g_test_add_func("/connection/healthy-says-nothing",
                    test_a_healthy_connection_says_nothing);
    g_test_add_func("/connection/notice-without-a-connection",
                    test_a_notice_without_a_connection_still_reads);
    g_test_add_func("/connection/versions-compare-as-numbers",
                    test_versions_compare_as_numbers);
    g_test_add_func("/connection/unreadable-version-is-not-a-verdict",
                    test_an_unreadable_version_is_not_a_verdict);
    g_test_add_func("/connection/mismatch-says-which-way-round",
                    test_the_mismatch_says_which_way_round);
    g_test_add_func("/connection/client-reconnects-on-its-own-context",
                    test_a_client_reconnects_on_its_own_context);
    g_test_add_func("/connection/reconnect-that-cannot-resume-says-so",
                    test_a_reconnect_that_cannot_resume_says_so);
    g_test_add_func("/connection/auto-reconnect-can-be-turned-off",
                    test_auto_reconnect_can_be_turned_off_mid_flight);
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
