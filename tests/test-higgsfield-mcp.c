/*
 * test-higgsfield-mcp.c - Higgsfield generation through a connector's sign-in
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The daemon's own MCP client and the generator on it, against a fake
 * streamable-HTTP server on a unix socket -- no network.  The server
 * answers the shapes Higgsfield's MCP server gave on 2026-11-22
 * (docs/higgsfield.org): a session id on initialize, the credential on
 * every request, `structuredContent` for the estimate, the job and its
 * status, and an event stream for one reply, with a notification ahead
 * of the answer, as streamable HTTP allows.
 *
 * Not here: the presigned upload and the result download, which go to
 * https URLs a unix-socket server cannot answer; the live run in
 * docs/office-custodial-team.org covers them.
 */

#include "clawtilla.h"
#include "clawt-test-util.h"

#include <string.h>

#define JOB_ID "050c8f07-d673-4006-96b3-c4c11d8f3d5c"

typedef struct {
    GMainContext *context;
    SoupServer   *server;
    gchar        *dir;
    gchar        *socket;
    guint         calls;
    guint         without_session;
    guint         without_credential;
    gboolean      refuse_all;      /* answer 401, as a signed-out server does */
    JsonObject   *last_params;     /* what generate_image was sent */
} Fake;

static void
reply_json(SoupServerMessage *message, const gchar *text)
{
    soup_server_message_set_status(message, 200, NULL);
    soup_server_message_set_response(message, "application/json", SOUP_MEMORY_COPY, text,
                                     strlen(text));
}

/* the answer as an event stream, with a notification first */
static void
reply_stream(SoupServerMessage *message, const gchar *text)
{
    g_autofree gchar *body = g_strdup_printf(
        "event: message\ndata: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/progress\","
        "\"params\":{\"progress\":1}}\n\nevent: message\ndata: %s\n\n", text);

    soup_server_message_set_status(message, 200, NULL);
    soup_server_message_set_response(message, "text/event-stream", SOUP_MEMORY_COPY, body,
                                     strlen(body));
}

static void
mcp_handler(SoupServer *server, SoupServerMessage *message, const char *path,
            GHashTable *query, gpointer user_data)
{
    Fake *fake = user_data;
    SoupMessageBody *request = soup_server_message_get_request_body(message);
    SoupMessageHeaders *headers = soup_server_message_get_request_headers(message);
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(GBytes) flat = soup_message_body_flatten(request);
    JsonObject *rpc;
    const gchar *method;
    gint64 id;
    g_autofree gchar *answer = NULL;

    (void)server;
    (void)path;
    (void)query;
    if (fake->refuse_all) {
        soup_server_message_set_status(message, 401, NULL);
        return;
    }
    if (g_strcmp0(soup_message_headers_get_one(headers, "Authorization"), "Bearer tok") != 0)
        fake->without_credential++;
    g_assert_true(json_parser_load_from_data(parser, g_bytes_get_data(flat, NULL),
                                             (gssize)g_bytes_get_size(flat), NULL));
    rpc = json_node_get_object(json_parser_get_root(parser));
    method = json_object_get_string_member(rpc, "method");
    id = json_object_get_int_member_with_default(rpc, "id", -1);

    if (g_strcmp0(method, "initialize") == 0) {
        soup_message_headers_replace(soup_server_message_get_response_headers(message),
                                     "Mcp-Session-Id", "session-1");
        answer = g_strdup_printf("{\"jsonrpc\":\"2.0\",\"id\":%" G_GINT64_FORMAT
                                 ",\"result\":{\"protocolVersion\":\"2025-06-18\","
                                 "\"capabilities\":{},\"serverInfo\":{\"name\":\"fake\"}}}", id);
        reply_json(message, answer);
        return;
    }
    if (g_strcmp0(soup_message_headers_get_one(headers, "Mcp-Session-Id"), "session-1") != 0)
        fake->without_session++;
    if (g_strcmp0(method, "notifications/initialized") == 0) {
        soup_server_message_set_status(message, 202, NULL);
        return;
    }

    /* tools/call */
    fake->calls++;
    {
        JsonObject *params = json_object_get_object_member(rpc, "params");
        const gchar *tool = json_object_get_string_member(params, "name");
        JsonObject *arguments = json_object_get_object_member(params, "arguments");

        if (g_strcmp0(tool, "estimate_image_cost") == 0) {
            answer = g_strdup_printf(
                "{\"jsonrpc\":\"2.0\",\"id\":%" G_GINT64_FORMAT ",\"result\":{\"content\":"
                "[{\"type\":\"text\",\"text\":\"1.5 credits\"}],\"structuredContent\":"
                "{\"cost\":{\"credits\":1.5,\"credits_exact\":1.5},\"quote_ref\":\"quote:x\"}}}",
                id);
            reply_stream(message, answer);
        } else if (g_strcmp0(tool, "generate_image") == 0) {
            g_clear_pointer(&fake->last_params, json_object_unref);
            fake->last_params = json_object_ref(json_object_get_object_member(arguments,
                                                                              "params"));
            answer = g_strdup_printf(
                "{\"jsonrpc\":\"2.0\",\"id\":%" G_GINT64_FORMAT ",\"result\":{\"content\":[],"
                "\"structuredContent\":{\"results\":[{\"id\":\"" JOB_ID "\","
                "\"status\":\"pending\"}]}}}", id);
            reply_json(message, answer);
        } else if (g_strcmp0(tool, "job_status") == 0) {
            /* the argument is camel case; anything else is the server's refusal */
            if (!json_object_has_member(arguments, "jobId")) {
                answer = g_strdup_printf(
                    "{\"jsonrpc\":\"2.0\",\"id\":%" G_GINT64_FORMAT ",\"result\":{\"isError\":"
                    "true,\"content\":[{\"type\":\"text\",\"text\":\"jobId: expected string\"}]}}",
                    id);
            } else {
                answer = g_strdup_printf(
                    "{\"jsonrpc\":\"2.0\",\"id\":%" G_GINT64_FORMAT ",\"result\":{\"content\":[],"
                    "\"structuredContent\":{\"generation\":{\"id\":\"" JOB_ID "\",\"status\":"
                    "\"completed\",\"results\":{\"rawUrl\":\"https://cdn.test/r.png\"}}}}}", id);
            }
            reply_json(message, answer);
        } else {
            answer = g_strdup_printf(
                "{\"jsonrpc\":\"2.0\",\"id\":%" G_GINT64_FORMAT ",\"result\":{\"isError\":true,"
                "\"content\":[{\"type\":\"text\",\"text\":\"no such tool\"}]}}", id);
            reply_json(message, answer);
        }
    }
}

static void
fake_setup(Fake *fake)
{
    g_autoptr(GSocketAddress) address = NULL;
    g_autoptr(GError) error = NULL;

    fake->context = g_main_context_new();
    g_main_context_push_thread_default(fake->context);
    fake->dir = g_dir_make_tmp("clawt-hfmcp-XXXXXX", NULL);
    fake->socket = g_build_filename(fake->dir, "mcp.sock", NULL);
    fake->server = soup_server_new(NULL, NULL);
    soup_server_add_handler(fake->server, NULL, mcp_handler, fake, NULL);
    address = g_unix_socket_address_new(fake->socket);
    g_assert_true(soup_server_listen(fake->server, address, 0, &error));
    g_assert_no_error(error);
}

static void
fake_teardown(Fake *fake)
{
    soup_server_disconnect(fake->server);
    g_clear_object(&fake->server);
    while (g_main_context_iteration(fake->context, FALSE))
        ;
    g_clear_pointer(&fake->last_params, json_object_unref);
    g_main_context_pop_thread_default(fake->context);
    g_main_context_unref(fake->context);
    clawt_test_remove_tree(fake->dir);
    g_free(fake->dir);
    g_free(fake->socket);
}

static ClawtMcpHttpClient *
client_for(Fake *fake)
{
    g_autoptr(GSocketAddress) address = g_unix_socket_address_new(fake->socket);
    g_autoptr(SoupSession) session = soup_session_new_with_options("remote-connectable",
                                                                   address, NULL);
    ClawtMcpHttpClient *client = clawt_mcp_http_client_new("hf", "http://mcp.test/mcp",
                                                           "Authorization", "Bearer tok");

    clawt_mcp_http_client_set_session(client, session);
    return client;
}

static void
on_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GAsyncResult **out = user_data;

    (void)source;
    *out = g_object_ref(result);
}

static GAsyncResult *
wait_for(Fake *fake, GAsyncResult **slot)
{
    gint64 deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;

    while (*slot == NULL) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_main_context_iteration(fake->context, TRUE);
    }
    return *slot;
}

/* ── The reply reader, as data ───────────────────────────────────── */

static void
test_reply_parsing(void)
{
    const gchar *json = "{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{}}";
    const gchar *stream =
        "event: message\ndata: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/x\"}\n\n"
        "event: message\ndata: {\"jsonrpc\":\"2.0\",\n"
        "data: \"id\":7,\"result\":{\"ok\":true}}\n\n";
    g_autoptr(JsonObject) a = NULL;
    g_autoptr(JsonObject) b = NULL;
    g_autoptr(GError) error = NULL;

    a = clawt_mcp_http_parse_reply(json, strlen(json), "application/json", 7, &error);
    g_assert_no_error(error);
    g_assert_nonnull(a);

    /* a notification first, and an answer split across two data lines */
    b = clawt_mcp_http_parse_reply(stream, strlen(stream), "text/event-stream", 7, &error);
    g_assert_no_error(error);
    g_assert_nonnull(b);
    g_assert_true(json_object_get_boolean_member(json_object_get_object_member(b, "result"),
                                                 "ok"));

    /* an answer to another request is not this one's */
    g_assert_null(clawt_mcp_http_parse_reply(json, strlen(json), "application/json", 8, &error));
    g_assert_nonnull(error);
}

/* ── What is sent ────────────────────────────────────────────────── */

static void
test_params(void)
{
    g_autoptr(AiMediaRequest) bare = ai_media_request_new(NULL, "a person");
    g_autoptr(AiMediaRequest) pictured = ai_media_request_new("gpt_image_2_6", "a person");
    g_autoptr(JsonObject) one = NULL;
    g_autoptr(JsonObject) two = NULL;
    JsonArray *medias;

    one = clawt_higgsfield_mcp_params_for(bare);
    g_assert_cmpstr(json_object_get_string_member(one, "model"), ==,
                    CLAWT_HIGGSFIELD_MCP_DEFAULT_MODEL);
    g_assert_false(json_object_has_member(one, "medias"));

    /* a picture goes as a media id with a role, never a URL */
    ai_media_request_set_image_url(pictured, "51eb6158-992d-4450-94d6-b2d59a8a7211");
    two = clawt_higgsfield_mcp_params_for(pictured);
    g_assert_cmpstr(json_object_get_string_member(two, "model"), ==, "gpt_image_2_6");
    medias = json_object_get_array_member(two, "medias");
    g_assert_cmpuint(json_array_get_length(medias), ==, 1);
    g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(medias, 0),
                                                  "value"), ==,
                    "51eb6158-992d-4450-94d6-b2d59a8a7211");
    g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(medias, 0),
                                                  "role"), ==, "image");
}

/* ── The generator end to end, against the fake ──────────────────── */

static void
test_estimate_submit_poll(void)
{
    Fake fake = { 0 };
    g_autoptr(ClawtMcpHttpClient) client = NULL;
    g_autoptr(ClawtHiggsfieldMcp) generator = NULL;
    g_autoptr(AiMediaRequest) request = ai_media_request_new(NULL, "a person");
    g_autoptr(GError) error = NULL;
    GAsyncResult *result = NULL;
    gint64 micro = 0;
    g_autofree gchar *job_id = NULL;
    g_autoptr(AiMediaJob) job = NULL;

    fake_setup(&fake);
    client = client_for(&fake);
    generator = clawt_higgsfield_mcp_new(client);

    /* the estimate is credits, read from an event stream; dollars are unknown */
    ai_media_generator_estimate_async(AI_MEDIA_GENERATOR(generator), request, NULL, on_done,
                                      &result);
    g_assert_true(ai_media_generator_estimate_finish(AI_MEDIA_GENERATOR(generator),
                                                     wait_for(&fake, &result), &micro, &error));
    g_assert_no_error(error);
    g_assert_cmpint(micro, ==, AI_MEDIA_COST_UNKNOWN);
    g_assert_cmpfloat(clawt_higgsfield_mcp_get_estimate_credits(generator), ==, 1.5);
    g_clear_object(&result);

    ai_media_generator_submit_async(AI_MEDIA_GENERATOR(generator), request, NULL, on_done,
                                    &result);
    job_id = ai_media_generator_submit_finish(AI_MEDIA_GENERATOR(generator),
                                              wait_for(&fake, &result), &error);
    g_assert_no_error(error);
    g_assert_cmpstr(job_id, ==, JOB_ID);
    g_assert_cmpstr(json_object_get_string_member(fake.last_params, "prompt"), ==, "a person");
    g_clear_object(&result);

    ai_media_generator_poll_async(AI_MEDIA_GENERATOR(generator), job_id, NULL, on_done,
                                  &result);
    job = ai_media_generator_poll_finish(AI_MEDIA_GENERATOR(generator),
                                         wait_for(&fake, &result), &error);
    g_assert_no_error(error);
    g_assert_cmpint(ai_media_job_get_state(job), ==, AI_MEDIA_JOB_STATE_DONE);
    g_assert_cmpuint(ai_media_job_get_n_result_urls(job), ==, 1);
    g_assert_cmpstr(ai_media_job_get_result_urls(job)[0], ==, "https://cdn.test/r.png");
    g_clear_object(&result);

    /* one session, initialized once, and the credential and its id on everything */
    g_assert_cmpuint(fake.calls, ==, 3);
    g_assert_cmpuint(fake.without_session, ==, 0);
    g_assert_cmpuint(fake.without_credential, ==, 0);

    g_clear_object(&generator);
    g_clear_object(&client);
    fake_teardown(&fake);
}

/* A signed-out server's 401 is an auth error that names the sign-in. */
static void
test_signed_out(void)
{
    Fake fake = { 0 };
    g_autoptr(ClawtMcpHttpClient) client = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonObject) outcome = NULL;
    GAsyncResult *result = NULL;

    fake_setup(&fake);
    fake.refuse_all = TRUE;
    client = client_for(&fake);
    clawt_mcp_http_client_call_tool_async(client, "balance", NULL, NULL, on_done, &result);
    outcome = clawt_mcp_http_client_call_tool_finish(client, wait_for(&fake, &result), &error);
    g_assert_null(outcome);
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_AUTH);
    g_assert_nonnull(strstr(error->message, "clawtilla connector connect hf"));
    g_clear_object(&result);
    g_clear_object(&client);
    fake_teardown(&fake);
}

gint
main(gint argc, gchar *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/higgsfield-mcp/reply-parsing", test_reply_parsing);
    g_test_add_func("/higgsfield-mcp/params", test_params);
    g_test_add_func("/higgsfield-mcp/estimate-submit-poll", test_estimate_submit_poll);
    g_test_add_func("/higgsfield-mcp/signed-out", test_signed_out);

    return g_test_run();
}
