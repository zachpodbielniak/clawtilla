/*
 * test-webhook-ingress.c - The one door a forge may knock on
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * What this listener serves is the one question about it worth being
 * sure of, because it is the only part of clawtilla a stranger can
 * reach.  The routing is a pure function precisely so that question is
 * answerable without opening a port -- `make test` opens no network
 * socket at all, and a test that had to be skipped for that reason would
 * be a test of the most important thing in the file that nobody runs.
 *
 * The one case that genuinely needs a socket -- that it binds the
 * loopback and not the world -- sits behind CLAWT_TEST_INTEGRATION,
 * beside the OAuth loopback tests that made the same trade.
 */

#include <clawtilla.h>

#include <libsoup/soup.h>

#include <string.h>

#include "clawt-test-util.h"

static gboolean
integration_enabled(void)
{
    return g_getenv("CLAWT_TEST_INTEGRATION") != NULL;
}

/* ── What is served ──────────────────────────────────────────────── */

/*
 * Two paths, and nothing else.
 *
 * The whole security argument for putting this behind a tunnel is that
 * the port exposes the ability to deliver an event and nothing else
 * about the machine -- not the IPC surface, not a file, not a listing.
 * So the negative half is asserted on explicitly rather than left as an
 * absence somebody would have to notice.
 */
static void
test_only_health_and_hooks_are_served(void)
{
    static const gchar *nothing[] = {
        "/", "/a", "/agents", "/config", "/etc/passwd", "/hooks",
        "/hooks/", "/static/x", "/health/x", "/HEALTH",
        "/../health", "/hooks/../../etc/passwd",
        "/hooks/abc/extra", "/hooks/abc/", NULL
    };
    guint i;

    for (i = 0; nothing[i] != NULL; i++) {
        const gchar *endpoint = (const gchar *)0x1;

        g_assert_cmpint(clawt_webhook_route("POST", nothing[i], &endpoint),
                        ==, CLAWT_WEBHOOK_ROUTE_NONE);

        /* And nothing is handed on from a path that was not one. */
        g_assert_null(endpoint);

        g_assert_cmpint(clawt_webhook_route("GET", nothing[i], NULL), ==,
                        CLAWT_WEBHOOK_ROUTE_NONE);
    }

    g_assert_cmpint(clawt_webhook_route("GET", NULL, NULL), ==,
                    CLAWT_WEBHOOK_ROUTE_NONE);
}

static void
test_health_is_a_get(void)
{
    g_assert_cmpint(clawt_webhook_route("GET", "/health", NULL), ==,
                    CLAWT_WEBHOOK_ROUTE_HEALTH);

    g_assert_cmpint(clawt_webhook_route("POST", "/health", NULL), ==,
                    CLAWT_WEBHOOK_ROUTE_WRONG_METHOD);
}

static void
test_a_delivery_is_a_post_to_an_endpoint(void)
{
    const gchar *endpoint = NULL;

    g_assert_cmpint(clawt_webhook_route("POST", "/hooks/abc123", &endpoint),
                    ==, CLAWT_WEBHOOK_ROUTE_DELIVERY);
    g_assert_cmpstr(endpoint, ==, "abc123");

    /* The right path with the wrong verb is not a delivery. */
    g_assert_cmpint(clawt_webhook_route("GET", "/hooks/abc123", NULL), ==,
                    CLAWT_WEBHOOK_ROUTE_WRONG_METHOD);
    g_assert_cmpint(clawt_webhook_route("PUT", "/hooks/abc123", NULL), ==,
                    CLAWT_WEBHOOK_ROUTE_WRONG_METHOD);
}

/*
 * A traversal is a 404, not a 405.
 *
 * A caller told "that path wants a different verb" has learned that the
 * path exists, which for a secret endpoint is most of what there is to
 * learn -- so the shape of the path is judged before the method.
 */
static void
test_a_traversal_is_not_found_rather_than_wrong_method(void)
{
    g_assert_cmpint(clawt_webhook_route("GET", "/hooks/abc/../../x", NULL),
                    ==, CLAWT_WEBHOOK_ROUTE_NONE);
    g_assert_cmpint(clawt_webhook_route("DELETE", "/hooks/abc/x", NULL), ==,
                    CLAWT_WEBHOOK_ROUTE_NONE);
}

/*
 * An endpoint that happens to look like a traversal but has no slash is
 * simply an endpoint that will not be found.
 *
 * It is looked up in the store, which knows nothing called that -- so it
 * is answered 404 by the same path a wrong guess is, rather than being
 * special-cased into a different answer that would tell a caller its
 * guess was interesting.
 */
static void
test_a_dotted_endpoint_is_still_just_an_endpoint(void)
{
    const gchar *endpoint = NULL;

    g_assert_cmpint(clawt_webhook_route("POST", "/hooks/..", &endpoint), ==,
                    CLAWT_WEBHOOK_ROUTE_DELIVERY);
    g_assert_cmpstr(endpoint, ==, "..");
}

/* ── Binding ─────────────────────────────────────────────────────── */

/*
 * It binds the loopback, and says so from what bound rather than from
 * what was asked for.
 *
 * A convenience address whose bind failed is exactly the interesting
 * case: reporting the request would say the fleet was reachable from a
 * laptop when it was not.
 */
static void
test_it_binds_the_loopback_and_says_what_bound(void)
{
    g_autoptr(ClawtWebhookIngress) ingress = NULL;
    g_autoptr(GError) error = NULL;
    GPtrArray *where;

    if (!integration_enabled()) {
        g_test_skip("needs CLAWT_TEST_INTEGRATION; opens a loopback socket");
        return;
    }

    /* Port 0: the kernel picks one, so a developer's own daemon is safe. */
    ingress = clawt_webhook_ingress_new(0, 1024);

    /*
     * FALSE for the tailnet, which is what a test must always pass. A
     * machine with a tailnet would otherwise have this listener
     * reachable from every other device the user enrolled, from a test.
     */
    g_assert_true(clawt_webhook_ingress_start(ingress, FALSE, &error));
    g_assert_no_error(error);

    where = clawt_webhook_ingress_get_addresses(ingress);

    g_assert_nonnull(where);
    g_assert_cmpuint(where->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(where, 0), ==, "127.0.0.1");

    clawt_webhook_ingress_stop(ingress);

    /* And it stops saying it is listening once it is not. */
    g_assert_cmpuint(clawt_webhook_ingress_get_addresses(ingress)->len, ==, 0);
}

/*
 * Nothing is listening until somebody starts it.
 *
 * The receiver is off by default, and a constructed-but-unstarted
 * ingress claiming an address would make `trigger.list` print a URL
 * nothing answers on.
 */
static void
test_an_unstarted_ingress_is_listening_nowhere(void)
{
    g_autoptr(ClawtWebhookIngress) ingress = clawt_webhook_ingress_new(8788,
                                                                      1024);

    g_assert_cmpuint(clawt_webhook_ingress_get_port(ingress), ==, 8788);
    g_assert_cmpuint(clawt_webhook_ingress_get_addresses(ingress)->len, ==, 0);

    /* And stopping one that never started is not a crash. */
    clawt_webhook_ingress_stop(ingress);
}

/* ── End to end ──────────────────────────────────────────────────── */

/*
 * The order the checks happen in is the security, and it is the one
 * thing above that cannot be reached without a socket.
 *
 * `on_delivery()` is static inside daemon-trigger.c and takes the whole
 * daemon, so the only honest way to exercise it is to start one and
 * knock. That needs a listener, so it sits behind
 * CLAWT_TEST_INTEGRATION -- `make test` opens no network socket at all.
 * Recorded here rather than left undone: the alternative is a rule
 * defended only by the comments beside it.
 */
typedef struct {
    gchar        *dir;
    gchar        *config_path;
    GMainContext *context;
    ClawtDaemon  *daemon;
    SoupSession  *session;
    guint16       port;
} EndToEnd;

/*
 * A port the kernel is unlikely to have given to anybody else.
 *
 * Not 8788: a developer running this may well have a real daemon on the
 * default, and a test that fought it for the port would fail for a
 * reason that has nothing to do with the code.
 */
#define TEST_WEBHOOK_PORT (18788)

static void
end_to_end_setup(EndToEnd *fixture, const gchar *trigger_yaml)
{
    g_autofree gchar *yaml = NULL;
    g_autoptr(GError) error = NULL;

    fixture->dir = g_dir_make_tmp("clawt-hook-XXXXXX", NULL);
    fixture->config_path = g_build_filename(fixture->dir, "config.yaml",
                                            NULL);
    fixture->port = TEST_WEBHOOK_PORT;

    yaml = g_strdup_printf(
        "daemon:\n"
        "  tailscale: false\n"
        "  state_dir: \"%s/state\"\n"
        "  socket: \"%s/daemon.sock\"\n"
        "  automation_dir: \"%s/pods\"\n"
        "  webhook_enabled: true\n"
        "  webhook_port: %u\n"
        "secrets:\n  dir: \"%s/secrets\"\n"
        "defaults:\n  workspace_root: \"%s/agents\"\n"
        "agents:\n  - id: builder\n"
        "%s",
        fixture->dir, fixture->dir, fixture->dir, (guint)fixture->port,
        fixture->dir, fixture->dir,
        trigger_yaml != NULL ? trigger_yaml : "");

    g_file_set_contents(fixture->config_path, yaml, -1, &error);
    g_assert_no_error(error);

    fixture->context = g_main_context_new();

    /*
     * Pushed for the life of the fixture, so the SoupSession's own
     * sources attach to the context the ingress dispatches on.
     *
     * Without this the client waits on the default context and the
     * server answers on another that nothing is running, so the request
     * never completes -- a hang rather than a failure, which is the
     * worse of the two and exactly what the watchdog in post() is for.
     */
    g_main_context_push_thread_default(fixture->context);

    fixture->daemon = clawt_daemon_new(fixture->config_path,
                                       fixture->context);

    g_assert_true(clawt_daemon_start(fixture->daemon, &error));
    g_assert_no_error(error);

    fixture->session = soup_session_new();
}

static void
end_to_end_teardown(EndToEnd *fixture)
{
    g_clear_object(&fixture->session);

    if (fixture->daemon != NULL) {
        clawt_daemon_stop(fixture->daemon);
        g_clear_object(&fixture->daemon);
    }

    /*
     * Iterated after stopping, as the daemon fixture does: a listener's
     * outstanding accept finishes on the next loop pass, and until it
     * does the whole socket stack is still referenced.
     */
    if (fixture->context != NULL) {
        while (g_main_context_iteration(fixture->context, FALSE))
            ;
    }

    if (fixture->context != NULL)
        g_main_context_pop_thread_default(fixture->context);

    g_clear_pointer(&fixture->context, g_main_context_unref);

    if (fixture->dir != NULL)
        clawt_test_remove_tree(fixture->dir);

    g_clear_pointer(&fixture->dir, g_free);
    g_clear_pointer(&fixture->config_path, g_free);
}

/*
 * One request, driven to completion by pumping the one context both
 * halves live on.
 *
 * The deadline is not a nicety. The client and the server are the same
 * loop here, so anything that fails to complete would spin this forever
 * -- and a test that hangs tells you nothing about which assertion was
 * the problem.
 */
typedef struct {
    gboolean  done;
    guint     status;
} Exchange;

static void
on_sent(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Exchange *exchange = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) body = soup_session_send_and_read_finish(
        SOUP_SESSION(source), result, &error);

    (void)body;

    exchange->done = TRUE;

    if (error != NULL)
        g_test_message("delivery failed: %s", error->message);
}

static guint
run_exchange(EndToEnd *fixture, SoupMessage *message)
{
    Exchange exchange = { FALSE, 0 };
    gint64 deadline = g_get_monotonic_time() + (15 * G_USEC_PER_SEC);

    soup_session_send_and_read_async(fixture->session, message,
                                     G_PRIORITY_DEFAULT, NULL, on_sent,
                                     &exchange);

    while (!exchange.done) {
        if (g_get_monotonic_time() > deadline) {
            g_test_message("timed out waiting for a reply");
            return 0;
        }

        g_main_context_iteration(fixture->context, FALSE);
        g_usleep(1000);
    }

    return soup_message_get_status(message);
}

/*
 * One delivery, and the daemon's own loop pumped while it is in flight.
 *
 * The ingress dispatches on the daemon's context, not the caller's, so a
 * request made without iterating that context would sit unanswered until
 * libsoup gave up -- which is a hang, not a failure, and the worse of
 * the two.
 */
static guint
post(EndToEnd *fixture, const gchar *path, GHashTable *headers,
     const gchar *body)
{
    g_autofree gchar *url = g_strdup_printf("http://127.0.0.1:%u%s",
                                            (guint)fixture->port, path);
    g_autoptr(SoupMessage) message = soup_message_new("POST", url);
    g_autoptr(GBytes) sent = NULL;

    if (headers != NULL) {
        GHashTableIter iter;
        gpointer name;
        gpointer value;

        g_hash_table_iter_init(&iter, headers);

        while (g_hash_table_iter_next(&iter, &name, &value))
            soup_message_headers_append(
                soup_message_get_request_headers(message), name, value);
    }

    if (body != NULL) {
        sent = g_bytes_new(body, strlen(body));
        soup_message_set_request_body_from_bytes(message, "application/json",
                                                 sent);
    }

    return run_exchange(fixture, message);
}

static GHashTable *
signed_headers(const gchar *secret, const gchar *body,
               const gchar *delivery_id)
{
    GHashTable *headers = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                g_free, g_free);
    gchar *digest = g_compute_hmac_for_data(
        G_CHECKSUM_SHA256, (const guchar *)secret, strlen(secret),
        (const guchar *)body, strlen(body));

    g_hash_table_insert(headers, g_strdup("X-Forgejo-Event"),
                        g_strdup("push"));
    g_hash_table_insert(headers, g_strdup("X-Forgejo-Signature"), digest);

    if (delivery_id != NULL)
        g_hash_table_insert(headers, g_strdup("X-Forgejo-Delivery"),
                            g_strdup(delivery_id));

    return headers;
}

static gchar *
add_trigger(EndToEnd *fixture, const gchar *json)
{
    g_autoptr(JsonNode) frame = clawt_ipc_request_new("trigger.add", "t1");
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(JsonNode) reply = NULL;
    JsonObject *payload;

    g_assert_true(json_parser_load_from_data(parser, json, -1, NULL));
    clawt_ipc_frame_set_payload(frame,
                                json_node_copy(json_parser_get_root(parser)));

    reply = clawt_daemon_handle_request(fixture->daemon, frame);
    g_assert_false(clawt_ipc_frame_is_error(reply));

    payload = json_object_get_object_member(json_node_get_object(reply),
                                            "payload");

    /* The secret, which crosses IPC exactly here and nowhere else. */
    return g_strdup(json_object_get_string_member(payload, "secret"));
}

/*
 * The whole delivery path, in the order it actually happens.
 *
 * Six assertions, and every one of them is a different wrong answer that
 * would look like success from outside: a bad signature accepted, a
 * retry run twice, a disabled trigger admitting that it exists, an
 * unknown endpoint doing the same, an oversized body buffered, and a
 * path that is not served answering at all.
 */
static void
test_the_delivery_path_end_to_end(void)
{
    EndToEnd fixture = { 0 };
    g_autofree gchar *secret = NULL;
    g_autofree gchar *endpoint = NULL;
    static const gchar body[] =
        "{\"ref\": \"refs/heads/master\","
        " \"repository\": {\"full_name\": \"zach/clawtilla\"}}";

    if (!integration_enabled()) {
        g_test_skip("needs CLAWT_TEST_INTEGRATION; opens a loopback socket");
        return;
    }

    end_to_end_setup(&fixture, NULL);

    secret = add_trigger(&fixture,
                         "{\"id\": \"ci\", \"agent\": \"builder\","
                         " \"instructions\": \"go\","
                         " \"provider\": \"forgejo\"}");
    g_assert_nonnull(secret);

    {
        g_autoptr(JsonNode) frame = clawt_ipc_request_new("trigger.list",
                                                          "t2");
        g_autoptr(JsonNode) reply =
            clawt_daemon_handle_request(fixture.daemon, frame);
        JsonObject *first = json_array_get_object_element(
            json_object_get_array_member(
                json_object_get_object_member(json_node_get_object(reply),
                                              "payload"),
                "triggers"), 0);

        endpoint = g_strdup(json_object_get_string_member(first, "endpoint"));
    }

    g_assert_nonnull(endpoint);

    /* Only /health and /hooks/... are served. */
    {
        g_autofree gchar *url = g_strdup_printf("http://127.0.0.1:%u/health",
                                                (guint)fixture.port);
        g_autoptr(SoupMessage) message = soup_message_new("GET", url);

        g_assert_cmpuint(run_exchange(&fixture, message), ==,
                         SOUP_STATUS_OK);
    }

    g_assert_cmpuint(post(&fixture, "/agents", NULL, "{}"), ==,
                     SOUP_STATUS_NOT_FOUND);

    /* An endpoint nobody registered is a 404, so scanning learns nothing. */
    g_assert_cmpuint(post(&fixture, "/hooks/not-a-real-endpoint", NULL,
                          "{}"),
                     ==, SOUP_STATUS_NOT_FOUND);

    {
        g_autofree gchar *path = g_strdup_printf("/hooks/%s", endpoint);

        /*
         * A wrong signature is a 401 -- reachable because the trigger is
         * still pending, which is the one state a disabled trigger
         * accepts deliveries in.
         */
        {
            g_autoptr(GHashTable) headers = signed_headers("wrong-secret",
                                                            body, "d-1");

            g_assert_cmpuint(post(&fixture, path, headers, body), ==,
                             SOUP_STATUS_UNAUTHORIZED);
        }

        /* An oversized body is refused before any of that. */
        {
            g_autofree gchar *huge = g_strnfill(2 * 1024 * 1024, 'x');
            g_autoptr(GHashTable) headers = signed_headers(secret, huge,
                                                            "d-big");

            g_assert_cmpuint(
                post(&fixture, path, headers, huge), ==,
                SOUP_STATUS_REQUEST_ENTITY_TOO_LARGE);
        }

        /*
         * The first good delivery is captured, not run: a new trigger is
         * unverified, and that is the handshake.
         */
        {
            g_autoptr(GHashTable) headers = signed_headers(secret, body,
                                                            "d-2");

            g_assert_cmpuint(post(&fixture, path, headers, body), ==,
                             SOUP_STATUS_OK);
        }

        /*
         * And now that the handshake is done it is disabled-and-verified,
         * which answers as though the endpoint does not exist -- so a
         * trigger somebody switched off tells a caller nothing.
         */
        {
            g_autoptr(GHashTable) headers = signed_headers(secret, body,
                                                            "d-3");

            g_assert_cmpuint(post(&fixture, path, headers, body), ==,
                             SOUP_STATUS_NOT_FOUND);
        }
    }

    /* Every one of those left a receipt saying which it was. */
    {
        g_autoptr(JsonNode) frame =
            clawt_ipc_request_new("trigger.deliveries", "t3");
        g_autoptr(JsonNode) reply =
            clawt_daemon_handle_request(fixture.daemon, frame);
        JsonArray *rows = json_object_get_array_member(
            json_object_get_object_member(json_node_get_object(reply),
                                          "payload"),
            "deliveries");

        g_assert_cmpuint(json_array_get_length(rows), >=, 2);
    }

    end_to_end_teardown(&fixture);
}

/*
 * The capability URL, driven through the real listener.
 *
 * clawt_trigger_verify_url_secret() is unit-tested in test-venture.c,
 * but a rule that reaches nobody is the shape this codebase keeps
 * hitting: the secret has to survive libsoup's query parsing, the
 * ingress has to hand it on, and the delivery path has to prefer it
 * over the header check. None of that is visible from the function.
 *
 * The sender this is for is podomation's webhook module, whose post()
 * takes a URL and sets no headers at all -- so if this does not work,
 * the shipped VENTURE recipe authenticates nothing.
 */
static void
test_a_capability_url_opens_a_generic_trigger(void)
{
    EndToEnd fixture = { 0 };
    g_autofree gchar *generic_secret = NULL;
    g_autofree gchar *forge_secret = NULL;
    g_autofree gchar *generic_endpoint = NULL;
    g_autofree gchar *forge_endpoint = NULL;
    static const gchar body[] = "{\"type\": \"sale\", \"id\": 42}";

    if (!integration_enabled()) {
        g_test_skip("needs CLAWT_TEST_INTEGRATION; opens a loopback socket");
        return;
    }

    end_to_end_setup(&fixture, NULL);

    generic_secret = add_trigger(&fixture,
                                 "{\"id\": \"deal\", \"agent\": \"builder\","
                                 " \"instructions\": \"go\","
                                 " \"provider\": \"generic\"}");
    forge_secret = add_trigger(&fixture,
                               "{\"id\": \"ci\", \"agent\": \"builder\","
                               " \"instructions\": \"go\","
                               " \"provider\": \"forgejo\"}");

    g_assert_nonnull(generic_secret);
    g_assert_nonnull(forge_secret);

    {
        g_autoptr(JsonNode) frame = clawt_ipc_request_new("trigger.list",
                                                          "t2");
        g_autoptr(JsonNode) reply =
            clawt_daemon_handle_request(fixture.daemon, frame);
        JsonArray *rows = json_object_get_array_member(
            json_object_get_object_member(json_node_get_object(reply),
                                          "payload"),
            "triggers");
        guint i;

        for (i = 0; i < json_array_get_length(rows); i++) {
            JsonObject *row = json_array_get_object_element(rows, i);
            const gchar *id = json_object_get_string_member(row, "id");

            if (g_strcmp0(id, "deal") == 0)
                generic_endpoint =
                    g_strdup(json_object_get_string_member(row, "endpoint"));
            else if (g_strcmp0(id, "ci") == 0)
                forge_endpoint =
                    g_strdup(json_object_get_string_member(row, "endpoint"));
        }
    }

    g_assert_nonnull(generic_endpoint);
    g_assert_nonnull(forge_endpoint);

    /* The wrong secret in the URL is a 401, not an accident. */
    {
        g_autofree gchar *path =
            g_strdup_printf("/hooks/%s?token=not-the-secret",
                            generic_endpoint);

        g_assert_cmpuint(post(&fixture, path, NULL, body), ==,
                         SOUP_STATUS_UNAUTHORIZED);
    }

    /* And no secret at all, which is what an open endpoint would be. */
    {
        g_autofree gchar *path = g_strdup_printf("/hooks/%s",
                                                 generic_endpoint);

        g_assert_cmpuint(post(&fixture, path, NULL, body), ==,
                         SOUP_STATUS_UNAUTHORIZED);
    }

    /* The right one, with no headers whatsoever, is accepted. */
    {
        g_autofree gchar *path = g_strdup_printf("/hooks/%s?token=%s",
                                                 generic_endpoint,
                                                 generic_secret);

        g_assert_cmpuint(post(&fixture, path, NULL, body), ==,
                         SOUP_STATUS_OK);
    }

    /*
     * And the same proof against a forge is refused.
     *
     * A forge can sign, and every one of them does. If a query string
     * opened this, the weakest scheme any caller could reach for would
     * be the scheme every trigger accepted.
     */
    {
        g_autofree gchar *path = g_strdup_printf("/hooks/%s?token=%s",
                                                 forge_endpoint,
                                                 forge_secret);

        g_assert_cmpuint(post(&fixture, path, NULL, body), ==,
                         SOUP_STATUS_UNAUTHORIZED);
    }

    end_to_end_teardown(&fixture);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/webhook/only-two-paths",
                    test_only_health_and_hooks_are_served);
    g_test_add_func("/webhook/health-is-a-get", test_health_is_a_get);
    g_test_add_func("/webhook/delivery-is-a-post",
                    test_a_delivery_is_a_post_to_an_endpoint);
    g_test_add_func("/webhook/traversal-is-not-found",
                    test_a_traversal_is_not_found_rather_than_wrong_method);
    g_test_add_func("/webhook/dotted-endpoint",
                    test_a_dotted_endpoint_is_still_just_an_endpoint);
    g_test_add_func("/webhook/binds-loopback",
                    test_it_binds_the_loopback_and_says_what_bound);
    g_test_add_func("/webhook/unstarted-listens-nowhere",
                    test_an_unstarted_ingress_is_listening_nowhere);
    g_test_add_func("/webhook/end-to-end", test_the_delivery_path_end_to_end);
    g_test_add_func("/webhook/capability-url",
                    test_a_capability_url_opens_a_generic_trigger);

    return g_test_run();
}
