/*
 * test-link.c - The daemon end of an agent connection
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The most valuable test here is the last one: it runs a real
 * LcClawtillaChannel against a real ClawtLinkServer over a real socket.
 * Both sides were written from the same protocol description, which is
 * exactly the situation where two plausible readings diverge and neither
 * side's own tests notice.
 */

#include <clawtilla.h>

#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>

#include "clawt-test-util.h"

typedef struct {
    gchar           *dir;
    gchar           *socket_path;
    ClawtLinkServer *server;
} Fixture;

static void
fixture_setup(Fixture *fixture)
{
    fixture->dir = g_dir_make_tmp("clawt-link-XXXXXX", NULL);
    fixture->socket_path = g_build_filename(fixture->dir, "agents.sock", NULL);
    fixture->server = clawt_link_server_new(fixture->socket_path);
}

static void
fixture_teardown(Fixture *fixture)
{
    if (fixture->server != NULL) {
        clawt_link_server_stop(fixture->server);
        g_clear_object(&fixture->server);
    }

    g_unlink(fixture->socket_path);
    clawt_test_remove_tree(fixture->dir);

    g_clear_pointer(&fixture->socket_path, g_free);
    g_clear_pointer(&fixture->dir, g_free);
}

static gboolean
pump_until(gboolean (*predicate)(gpointer), gpointer data, guint max_ms)
{
    guint waited = 0;

    while (!predicate(data) && waited < max_ms) {
        g_main_context_iteration(NULL, FALSE);
        g_usleep(1000);
        waited++;
    }

    return predicate(data);
}

/* ── A hand-driven client, so the tests can send exactly what they mean ── */

typedef struct {
    GSocketConnection *connection;
    GDataInputStream  *input;
    GOutputStream     *output;
} Client;

static gboolean
client_connect(Client *client, const gchar *path)
{
    g_autoptr(GSocketClient) socket_client = g_socket_client_new();
    g_autoptr(GSocketAddress) address = g_unix_socket_address_new(path);
    g_autoptr(GError) error = NULL;

    client->connection = g_socket_client_connect(
        socket_client, G_SOCKET_CONNECTABLE(address), NULL, &error);

    if (client->connection == NULL)
        return FALSE;

    client->input = g_data_input_stream_new(
        g_io_stream_get_input_stream(G_IO_STREAM(client->connection)));
    g_data_input_stream_set_newline_type(client->input,
                                         G_DATA_STREAM_NEWLINE_TYPE_LF);
    client->output =
        g_io_stream_get_output_stream(G_IO_STREAM(client->connection));

    return TRUE;
}

static void
client_close(Client *client)
{
    g_clear_object(&client->input);
    client->output = NULL;

    if (client->connection != NULL) {
        g_io_stream_close(G_IO_STREAM(client->connection), NULL, NULL);
        g_clear_object(&client->connection);
    }
}

static void
client_send(Client *client, const gchar *json)
{
    g_autoptr(GError) error = NULL;

    g_output_stream_write_all(client->output, json, strlen(json), NULL, NULL,
                              &error);
    g_output_stream_write_all(client->output, "\n", 1, NULL, NULL, &error);
    g_assert_no_error(error);
}

typedef struct {
    gchar    *line;
    gsize     length;
    gboolean  done;
} ReadResult;

static void
on_client_line(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ReadResult *read = user_data;
    g_autoptr(GError) error = NULL;

    read->line = g_data_input_stream_read_line_finish(
        G_DATA_INPUT_STREAM(source), result, &read->length, &error);
    read->done = TRUE;
}

static gboolean
read_is_done(gpointer data)
{
    return ((ReadResult *)data)->done;
}

/*
 * Reads one frame, pumping the main loop while it waits.
 *
 * Asynchronous rather than blocking, for two reasons.  The server lives in
 * this same process and needs main-loop iterations to notice a request and
 * answer it, so a blocking read waits for a reply that can never be
 * written.  And checking "is the socket readable" first is not enough: the
 * bytes waiting may be half a line, and the blocking refill then stalls
 * anyway -- which is exactly how this deadlocked the first time.
 *
 * Returns NULL on timeout, so a broken link fails as a test rather than
 * hanging the suite.
 */
static JsonObject *
client_read(Client *client, gchar **out_kind)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(GError) error = NULL;
    ReadResult read = { NULL, 0, FALSE };
    JsonObject *envelope;

    g_data_input_stream_read_line_async(client->input, G_PRIORITY_DEFAULT,
                                        NULL, on_client_line, &read);

    if (!pump_until(read_is_done, &read, 3000))
        return NULL;

    if (read.line == NULL)
        return NULL;

    if (!json_parser_load_from_data(parser, read.line, (gssize)read.length,
                                    &error)) {
        g_free(read.line);
        return NULL;
    }

    g_free(read.line);

    envelope = json_node_get_object(json_parser_get_root(parser));
    if (envelope == NULL)
        return NULL;

    if (out_kind != NULL)
        *out_kind = g_strdup(json_object_get_string_member(envelope, "kind"));

    return json_object_ref(envelope);
}


/* ── Tests ───────────────────────────────────────────────────────── */

static void
test_server_starts_and_restricts_the_socket(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    GStatBuf st;

    fixture_setup(&fixture);

    g_assert_true(clawt_link_server_start(fixture.server, &error));
    g_assert_no_error(error);
    g_assert_true(g_file_test(fixture.socket_path, G_FILE_TEST_EXISTS));

    /* Everything an agent may do goes through here, so nobody else gets in. */
    g_assert_cmpint(g_stat(fixture.socket_path, &st), ==, 0);
    g_assert_cmpint(st.st_mode & 0077, ==, 0);

    fixture_teardown(&fixture);
}

/*
 * A socket left by a daemon that crashed must not stop the next one, but a
 * socket a LIVE daemon is using must: unlinking that would leave its agents
 * talking to a path that no longer exists.
 */
static void
test_stale_socket_is_cleared_but_live_one_is_not(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtLinkServer) second = NULL;

    fixture_setup(&fixture);

    /* A file where the socket goes, with nothing listening: stale. */
    g_assert_true(g_file_set_contents(fixture.socket_path, "", -1, &error));
    g_assert_true(clawt_link_server_start(fixture.server, &error));
    g_assert_no_error(error);

    /* Now a second server on the same path must refuse. */
    second = clawt_link_server_new(fixture.socket_path);
    g_assert_false(clawt_link_server_start(second, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_ALREADY_EXISTS);

    fixture_teardown(&fixture);
}

typedef struct {
    Fixture *fixture;
    gchar   *added;
    gchar   *removed;
    gchar   *last_body;
    gchar   *last_sender;
} Capture;

static void
on_link_added(ClawtLinkServer *server, const gchar *agent_id, gpointer data)
{
    Capture *capture = data;

    g_free(capture->added);
    capture->added = g_strdup(agent_id);
}

static void
on_link_removed(ClawtLinkServer *server, const gchar *agent_id, gpointer data)
{
    Capture *capture = data;

    g_free(capture->removed);
    capture->removed = g_strdup(agent_id);
}

static void
on_server_message(ClawtLinkServer *server, const gchar *agent_id,
                  const gchar *room_id, const gchar *body,
                  const gchar *thread_id, gpointer data)
{
    Capture *capture = data;

    g_free(capture->last_body);
    g_free(capture->last_sender);
    capture->last_body = g_strdup(body);
    capture->last_sender = g_strdup(agent_id);
}

static gboolean
have_added(gpointer data) { return ((Capture *)data)->added != NULL; }

static gboolean
have_removed(gpointer data) { return ((Capture *)data)->removed != NULL; }

static gboolean
have_body(gpointer data) { return ((Capture *)data)->last_body != NULL; }

static void
capture_clear(Capture *capture)
{
    g_clear_pointer(&capture->added, g_free);
    g_clear_pointer(&capture->removed, g_free);
    g_clear_pointer(&capture->last_body, g_free);
    g_clear_pointer(&capture->last_sender, g_free);
}

static void
test_handshake_registers_the_agent(void)
{
    Fixture fixture = { 0 };
    Capture capture = { 0 };
    Client client = { 0 };
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonObject) welcome = NULL;
    g_autofree gchar *kind = NULL;

    fixture_setup(&fixture);
    capture.fixture = &fixture;

    g_signal_connect(fixture.server, "link-added",
                     G_CALLBACK(on_link_added), &capture);
    g_assert_true(clawt_link_server_start(fixture.server, &error));

    g_assert_true(client_connect(&client, fixture.socket_path));
    client_send(&client,
        "{\"v\":1,\"kind\":\"control.hello\",\"payload\":"
        "{\"agent_id\":\"chief\",\"agent_name\":\"Chief\"}}");

    g_assert_true(pump_until(have_added, &capture, 2000));
    g_assert_cmpstr(capture.added, ==, "chief");

    welcome = client_read(&client, &kind);
    g_assert_nonnull(welcome);
    g_assert_cmpstr(kind, ==, "control.welcome");

    g_assert_nonnull(clawt_link_server_get_link(fixture.server, "chief"));
    g_assert_cmpuint(clawt_link_server_count_links(fixture.server), ==, 1);

    client_close(&client);
    capture_clear(&capture);
    fixture_teardown(&fixture);
}

static gboolean
auth_only_chief(const gchar *agent_id, const gchar *token, gpointer data)
{
    (void)data;
    return g_strcmp0(agent_id, "chief") == 0 &&
           g_strcmp0(token, "correct-token") == 0;
}

/*
 * The socket's permissions keep other users out; the token keeps agents on
 * this machine from claiming each other's identity and reading each other's
 * mail.
 */
static void
test_bad_token_is_refused(void)
{
    Fixture fixture = { 0 };
    Capture capture = { 0 };
    Client client = { 0 };
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonObject) response = NULL;
    g_autofree gchar *kind = NULL;

    fixture_setup(&fixture);
    g_signal_connect(fixture.server, "link-added",
                     G_CALLBACK(on_link_added), &capture);
    clawt_link_server_set_auth_func(fixture.server, auth_only_chief, NULL, NULL);
    g_assert_true(clawt_link_server_start(fixture.server, &error));

    g_assert_true(client_connect(&client, fixture.socket_path));
    client_send(&client,
        "{\"v\":1,\"kind\":\"control.hello\",\"payload\":"
        "{\"agent_id\":\"chief\",\"token\":\"wrong\"}}");

    response = client_read(&client, &kind);
    g_assert_nonnull(response);
    g_assert_cmpstr(kind, ==, "control.error");

    g_assert_null(capture.added);
    g_assert_cmpuint(clawt_link_server_count_links(fixture.server), ==, 0);

    client_close(&client);
    capture_clear(&capture);
    fixture_teardown(&fixture);
}

/* Chat before identifying has nowhere to be routed and nobody to be from. */
static void
test_message_before_hello_is_refused(void)
{
    Fixture fixture = { 0 };
    Capture capture = { 0 };
    Client client = { 0 };
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonObject) response = NULL;
    g_autofree gchar *kind = NULL;

    fixture_setup(&fixture);
    g_signal_connect(fixture.server, "message",
                     G_CALLBACK(on_server_message), &capture);
    g_assert_true(clawt_link_server_start(fixture.server, &error));

    g_assert_true(client_connect(&client, fixture.socket_path));
    client_send(&client,
        "{\"v\":1,\"kind\":\"chat.message_out\",\"payload\":"
        "{\"body\":\"who am I?\"}}");

    response = client_read(&client, &kind);
    g_assert_cmpstr(kind, ==, "control.error");
    g_assert_null(capture.last_body);

    client_close(&client);
    capture_clear(&capture);
    fixture_teardown(&fixture);
}

/*
 * A reconnect replaces the previous link.  Keeping both would send messages
 * to whichever the table happened to hold, with the stale one never
 * noticing it had been superseded.
 */
static void
test_reconnect_replaces_the_previous_link(void)
{
    Fixture fixture = { 0 };
    Capture capture = { 0 };
    Client first = { 0 };
    Client second = { 0 };
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonObject) welcome_one = NULL;
    g_autoptr(JsonObject) welcome_two = NULL;

    fixture_setup(&fixture);
    g_signal_connect(fixture.server, "link-added",
                     G_CALLBACK(on_link_added), &capture);
    g_assert_true(clawt_link_server_start(fixture.server, &error));

    g_assert_true(client_connect(&first, fixture.socket_path));
    client_send(&first,
        "{\"v\":1,\"kind\":\"control.hello\",\"payload\":{\"agent_id\":\"chief\"}}");
    g_assert_true(pump_until(have_added, &capture, 2000));
    welcome_one = client_read(&first, NULL);

    g_clear_pointer(&capture.added, g_free);

    g_assert_true(client_connect(&second, fixture.socket_path));
    client_send(&second,
        "{\"v\":1,\"kind\":\"control.hello\",\"payload\":{\"agent_id\":\"chief\"}}");
    g_assert_true(pump_until(have_added, &capture, 2000));
    welcome_two = client_read(&second, NULL);

    /* Still exactly one link for that agent, and it is the new one. */
    g_assert_cmpuint(clawt_link_server_count_links(fixture.server), ==, 1);
    g_assert_true(clawt_link_is_open(
        clawt_link_server_get_link(fixture.server, "chief")));

    client_close(&first);
    client_close(&second);
    capture_clear(&capture);
    fixture_teardown(&fixture);
}

/* A second hello on a live link would let a connection change identity
 * mid-stream, delivering one agent's queued messages to another. */
static void
test_second_hello_is_refused(void)
{
    Fixture fixture = { 0 };
    Capture capture = { 0 };
    Client client = { 0 };
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonObject) welcome = NULL;
    g_autoptr(JsonObject) refusal = NULL;
    g_autofree gchar *kind = NULL;

    fixture_setup(&fixture);
    g_signal_connect(fixture.server, "link-added",
                     G_CALLBACK(on_link_added), &capture);
    g_assert_true(clawt_link_server_start(fixture.server, &error));

    g_assert_true(client_connect(&client, fixture.socket_path));
    client_send(&client,
        "{\"v\":1,\"kind\":\"control.hello\",\"payload\":{\"agent_id\":\"chief\"}}");
    g_assert_true(pump_until(have_added, &capture, 2000));
    welcome = client_read(&client, NULL);

    client_send(&client,
        "{\"v\":1,\"kind\":\"control.hello\",\"payload\":{\"agent_id\":\"impostor\"}}");

    refusal = client_read(&client, &kind);
    g_assert_cmpstr(kind, ==, "control.error");
    g_assert_null(clawt_link_server_get_link(fixture.server, "impostor"));

    client_close(&client);
    capture_clear(&capture);
    fixture_teardown(&fixture);
}

/* Hostile and malformed input must not take the link down. */
static void
test_malformed_frames_do_not_drop_the_link(void)
{
    Fixture fixture = { 0 };
    Capture capture = { 0 };
    Client client = { 0 };
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonObject) welcome = NULL;
    guint i;
    static const gchar *hostile[] = {
        "{ not json",
        "[]",                                    /* not an object */
        "{\"v\":99,\"kind\":\"chat.message_out\"}", /* wrong version */
        "{\"v\":1}",                             /* no kind */
        "{\"v\":1,\"kind\":\"future.invention\",\"payload\":{}}",
        "",                                      /* empty line */
        NULL
    };

    fixture_setup(&fixture);
    g_signal_connect(fixture.server, "link-added",
                     G_CALLBACK(on_link_added), &capture);
    g_signal_connect(fixture.server, "message",
                     G_CALLBACK(on_server_message), &capture);
    g_assert_true(clawt_link_server_start(fixture.server, &error));

    g_assert_true(client_connect(&client, fixture.socket_path));
    client_send(&client,
        "{\"v\":1,\"kind\":\"control.hello\",\"payload\":{\"agent_id\":\"chief\"}}");
    g_assert_true(pump_until(have_added, &capture, 2000));
    welcome = client_read(&client, NULL);

    for (i = 0; hostile[i] != NULL; i++)
        client_send(&client, hostile[i]);

    /* And a good message still gets through afterwards. */
    client_send(&client,
        "{\"v\":1,\"kind\":\"chat.message_out\",\"payload\":"
        "{\"room_id\":\"r\",\"body\":\"still here\"}}");

    g_assert_true(pump_until(have_body, &capture, 2000));
    g_assert_cmpstr(capture.last_body, ==, "still here");
    g_assert_cmpstr(capture.last_sender, ==, "chief");

    client_close(&client);
    capture_clear(&capture);
    fixture_teardown(&fixture);
}

/* A disconnect must be noticed, or messages route into a dead socket. */
static void
test_disconnect_removes_the_link(void)
{
    Fixture fixture = { 0 };
    Capture capture = { 0 };
    Client client = { 0 };
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonObject) welcome = NULL;

    fixture_setup(&fixture);
    g_signal_connect(fixture.server, "link-added",
                     G_CALLBACK(on_link_added), &capture);
    g_signal_connect(fixture.server, "link-removed",
                     G_CALLBACK(on_link_removed), &capture);
    g_assert_true(clawt_link_server_start(fixture.server, &error));

    g_assert_true(client_connect(&client, fixture.socket_path));
    client_send(&client,
        "{\"v\":1,\"kind\":\"control.hello\",\"payload\":{\"agent_id\":\"chief\"}}");
    g_assert_true(pump_until(have_added, &capture, 2000));
    welcome = client_read(&client, NULL);

    client_close(&client);

    g_assert_true(pump_until(have_removed, &capture, 3000));
    g_assert_cmpstr(capture.removed, ==, "chief");
    g_assert_cmpuint(clawt_link_server_count_links(fixture.server), ==, 0);

    capture_clear(&capture);
    fixture_teardown(&fixture);
}

/* Delivery reaches the agent as a chat.message_in frame. */
static void
test_deliver_reaches_the_agent(void)
{
    Fixture fixture = { 0 };
    Capture capture = { 0 };
    Client client = { 0 };
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonObject) welcome = NULL;
    g_autoptr(JsonObject) delivered = NULL;
    g_autofree gchar *kind = NULL;
    JsonObject *payload;

    fixture_setup(&fixture);
    g_signal_connect(fixture.server, "link-added",
                     G_CALLBACK(on_link_added), &capture);
    g_assert_true(clawt_link_server_start(fixture.server, &error));

    g_assert_true(client_connect(&client, fixture.socket_path));
    client_send(&client,
        "{\"v\":1,\"kind\":\"control.hello\",\"payload\":{\"agent_id\":\"chief\"}}");
    g_assert_true(pump_until(have_added, &capture, 2000));
    welcome = client_read(&client, NULL);

    g_assert_true(clawt_link_deliver(
        clawt_link_server_get_link(fixture.server, "chief"),
        "standup", "researcher", "Researcher", "summary attached", NULL,
        &error));
    g_assert_no_error(error);

    delivered = client_read(&client, &kind);
    g_assert_cmpstr(kind, ==, "chat.message_in");

    payload = json_object_get_object_member(delivered, "payload");
    g_assert_cmpstr(json_object_get_string_member(payload, "body"),
                    ==, "summary attached");
    g_assert_cmpstr(json_object_get_string_member(payload, "sender"),
                    ==, "researcher");

    client_close(&client);
    capture_clear(&capture);
    fixture_teardown(&fixture);
}

/*
 * The one that matters: a real libreclaw channel against a real link
 * server.  Both were written from the same protocol description, which is
 * exactly where two plausible readings diverge without either side's own
 * tests noticing.
 */
static void
test_real_channel_talks_to_the_server(void)
{
    Fixture fixture = { 0 };
    Capture capture = { 0 };
    g_autoptr(LcClawtillaChannel) channel = NULL;
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture);
    g_signal_connect(fixture.server, "link-added",
                     G_CALLBACK(on_link_added), &capture);
    g_signal_connect(fixture.server, "message",
                     G_CALLBACK(on_server_message), &capture);
    g_assert_true(clawt_link_server_start(fixture.server, &error));

    channel = lc_clawtilla_channel_new(fixture.socket_path, "chief");
    lc_clawtilla_channel_set_agent_name(channel, "Chief of Staff");
    lc_channel_connect_async(LC_CHANNEL(channel), NULL, NULL, NULL);

    /* The handshake completes without either side being told what to expect. */
    g_assert_true(pump_until(have_added, &capture, 3000));
    g_assert_cmpstr(capture.added, ==, "chief");

    /* The agent's own name reached the server through the handshake. */
    g_assert_cmpstr(
        clawt_link_get_agent_name(
            clawt_link_server_get_link(fixture.server, "chief")),
        ==, "Chief of Staff");

    /* Agent -> daemon. */
    {
        g_autoptr(LcOutboundMessage) message =
            lc_outbound_message_new("clawtilla", "standup", NULL,
                                    "reporting in", NULL, NULL);

        lc_channel_send_message_async(LC_CHANNEL(channel), message, NULL,
                                      NULL, NULL);
        g_assert_true(pump_until(have_body, &capture, 3000));
        g_assert_cmpstr(capture.last_body, ==, "reporting in");
        g_assert_cmpstr(capture.last_sender, ==, "chief");
    }

    capture_clear(&capture);
    fixture_teardown(&fixture);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/link/server-starts", test_server_starts_and_restricts_the_socket);
    g_test_add_func("/link/stale-socket", test_stale_socket_is_cleared_but_live_one_is_not);
    g_test_add_func("/link/handshake", test_handshake_registers_the_agent);
    g_test_add_func("/link/bad-token", test_bad_token_is_refused);
    g_test_add_func("/link/message-before-hello", test_message_before_hello_is_refused);
    g_test_add_func("/link/reconnect-replaces", test_reconnect_replaces_the_previous_link);
    g_test_add_func("/link/second-hello", test_second_hello_is_refused);
    g_test_add_func("/link/malformed-frames", test_malformed_frames_do_not_drop_the_link);
    g_test_add_func("/link/disconnect", test_disconnect_removes_the_link);
    g_test_add_func("/link/deliver", test_deliver_reaches_the_agent);
    g_test_add_func("/link/real-channel", test_real_channel_talks_to_the_server);

    return g_test_run();
}
