/*
 * test-browser-live.c - Secrets typed into a real page by a real gsurf
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * tests/test-browser.c checks the secret path above gsurf's control
 * socket with a fake standing in for gsurf, and the fake can only say
 * what it was sent.  This checks what a fake cannot: that the password
 * and the one-time code gsurf types arrive at a login form's server as
 * the form's fields, that gsurf refuses a picture of the page between
 * the fill and the navigation, that the hold lifts on the next page, and
 * that the host a fill is checked against is the one the real browser
 * reports.
 *
 * The login form is served here, by a SoupServer on 127.0.0.1 in a
 * thread of its own, so the test needs no network.
 *
 * Behind CLAWT_TEST_INTEGRATION=1, and only in a GSURF=1 build: it needs
 * Xvfb and gsurf, and starts real processes.
 */

#include <clawtilla.h>
#include <libsoup/soup.h>

#include "clawt-test-util.h"

#include <string.h>

#define SECRET_VALUE "hunter2-live-never-shown"
#define TOTP_SEED "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ"

/* How long one browser tool may take: a cold WebKit start is slow. */
#define TOOL_SECONDS (60)

static const gchar LOGIN_PAGE[] =
    "<!doctype html><html><head><title>Live login</title></head><body>"
    "<form method=\"post\" action=\"/next\">"
    "<input id=\"pw\" name=\"pw\" type=\"password\">"
    "<input id=\"otp\" name=\"otp\" autocomplete=\"one-time-code\">"
    "<button type=\"submit\">Sign in</button>"
    "</form></body></html>";

static const gchar NEXT_PAGE[] =
    "<!doctype html><html><head><title>Signed in</title></head>"
    "<body><p>Welcome back.</p></body></html>";

/* ── The login form's server ────────────────────────────────────────── */

typedef struct {
    GThread      *thread;
    GMainContext *context;
    GMainLoop    *loop;
    SoupServer   *server;
    guint         port;
    GMutex        lock;
    GCond         ready;
    gboolean      started;
    gchar        *posted;   /* the last body POSTed to /next */
} Site;

static void
on_request(SoupServer *server, SoupServerMessage *message, const char *path,
           GHashTable *query, gpointer user_data)
{
    Site *site = user_data;

    (void)server;
    (void)query;

    if (g_strcmp0(path, "/login") == 0) {
        soup_server_message_set_status(message, SOUP_STATUS_OK, NULL);
        soup_server_message_set_response(message, "text/html", SOUP_MEMORY_STATIC,
                                         LOGIN_PAGE, strlen(LOGIN_PAGE));
        return;
    }
    if (g_strcmp0(path, "/next") == 0) {
        SoupMessageBody *body = soup_server_message_get_request_body(message);
        g_autoptr(GBytes) bytes = soup_message_body_flatten(body);

        g_mutex_lock(&site->lock);
        g_free(site->posted);
        site->posted = g_strndup(g_bytes_get_data(bytes, NULL), g_bytes_get_size(bytes));
        g_mutex_unlock(&site->lock);

        soup_server_message_set_status(message, SOUP_STATUS_OK, NULL);
        soup_server_message_set_response(message, "text/html", SOUP_MEMORY_STATIC,
                                         NEXT_PAGE, strlen(NEXT_PAGE));
        return;
    }
    soup_server_message_set_status(message, SOUP_STATUS_NOT_FOUND, NULL);
}

static gpointer
site_thread(gpointer data)
{
    Site *site = data;
    g_autoptr(GError) error = NULL;
    GSList *uris;

    g_main_context_push_thread_default(site->context);
    site->server = soup_server_new(NULL, NULL);
    soup_server_add_handler(site->server, NULL, on_request, site, NULL);
    soup_server_listen_local(site->server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error);
    g_assert_no_error(error);

    uris = soup_server_get_uris(site->server);
    site->port = (guint)g_uri_get_port(uris->data);
    g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);

    g_mutex_lock(&site->lock);
    site->started = TRUE;
    g_cond_signal(&site->ready);
    g_mutex_unlock(&site->lock);

    g_main_loop_run(site->loop);

    g_clear_object(&site->server);
    g_main_context_pop_thread_default(site->context);
    return NULL;
}

static void
site_start(Site *site)
{
    g_mutex_init(&site->lock);
    g_cond_init(&site->ready);
    site->context = g_main_context_new();
    site->loop = g_main_loop_new(site->context, FALSE);
    site->thread = g_thread_new("login-site", site_thread, site);

    g_mutex_lock(&site->lock);
    while (!site->started)
        g_cond_wait(&site->ready, &site->lock);
    g_mutex_unlock(&site->lock);
}

static gboolean
quit_loop(gpointer data)
{
    g_main_loop_quit(data);
    return G_SOURCE_REMOVE;
}

static void
site_stop(Site *site)
{
    g_main_context_invoke(site->context, quit_loop, site->loop);
    g_thread_join(site->thread);
    g_main_loop_unref(site->loop);
    g_main_context_unref(site->context);
    g_free(site->posted);
    g_mutex_clear(&site->lock);
    g_cond_clear(&site->ready);
}

static gchar *
site_dup_posted(Site *site)
{
    gchar *out;

    g_mutex_lock(&site->lock);
    out = g_strdup(site->posted);
    g_mutex_unlock(&site->lock);
    return out;
}

/* ── The daemon's half, without a daemon ────────────────────────────── */

typedef struct {
    gchar               *dir;
    ClawtConfig         *config;
    ClawtAgentManager   *agents;
    ClawtTaskManager    *tasks;
    ClawtLoopGuard      *guard;
    ClawtMcpTools       *tools;
    ClawtBrowserManager *browsers;
    GMainContext        *context;
    GPtrArray           *heard;   /* every reply the agent read, as JSON */
} Fixture;

static void
write_secret(const gchar *dir, const gchar *name, const gchar *value)
{
    g_autofree gchar *path = g_build_filename(dir, name, NULL);
    g_autoptr(GError) error = NULL;

    g_file_set_contents_full(path, value, -1, G_FILE_SET_CONTENTS_NONE, 0600, &error);
    g_assert_no_error(error);
}

static void
fixture_setup(Fixture *fixture)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *yaml = NULL;
    g_autofree gchar *root = NULL;

    /* Under XDG_RUNTIME_DIR when there is one: socket paths are short. */
    fixture->dir = g_build_filename(g_get_user_runtime_dir(), "clawt-live-XXXXXX", NULL);
    g_assert_nonnull(g_mkdtemp(fixture->dir));
    write_secret(fixture->dir, "site-pass", SECRET_VALUE);
    write_secret(fixture->dir, "site-2fa", TOTP_SEED);

    yaml = g_strdup_printf("daemon:\n  state_dir: \"%s/state\"\n"
                           "defaults:\n  workspace_root: \"%s/agents\"\n"
                           "secrets:\n  dir: \"%s\"\n"
                           "vault:\n"
                           "  - name: site-pass\n"
                           "    secret: {file: site-pass}\n"
                           "    sites: [127.0.0.1]\n"
                           "  - name: site-2fa\n"
                           "    kind: totp\n"
                           "    secret: {file: site-2fa}\n"
                           "    sites: [127.0.0.1]\n"
                           "agents:\n"
                           "  - id: keeper\n"
                           "    browser:\n      mode: own\n"
                           "      width: 800\n      height: 600\n"
                           "      secrets: [site-pass, site-2fa]\n",
                           fixture->dir, fixture->dir, fixture->dir);
    fixture->config = clawt_config_load_from_string(yaml, &error);
    g_assert_no_error(error);

    fixture->agents = clawt_agent_manager_new(fixture->config);
    clawt_agent_manager_load(fixture->agents, NULL);
    fixture->tasks = clawt_task_manager_new();
    fixture->guard = clawt_loop_guard_new();
    fixture->context = g_main_context_new();
    fixture->tools = clawt_mcp_tools_new(fixture->agents, fixture->tasks, fixture->guard);
    clawt_mcp_tools_set_main_context(fixture->tools, fixture->context);

    /* No transport set: the manager starts a real Xvfb and gsurf. */
    root = g_build_filename(fixture->dir, "browsers", NULL);
    fixture->browsers = clawt_browser_manager_new(root);
    clawt_mcp_tools_set_browsers(fixture->tools, fixture->browsers);
    fixture->heard = g_ptr_array_new_with_free_func(g_free);
}

static void
fixture_teardown(Fixture *fixture)
{
    clawt_browser_manager_stop_all(fixture->browsers);
    g_clear_object(&fixture->tools);
    g_clear_object(&fixture->browsers);
    g_clear_object(&fixture->guard);
    g_clear_object(&fixture->tasks);
    g_clear_object(&fixture->agents);
    g_clear_object(&fixture->config);
    g_clear_pointer(&fixture->context, g_main_context_unref);
    g_ptr_array_unref(fixture->heard);
    clawt_test_remove_tree(fixture->dir);
    g_free(fixture->dir);
}

typedef struct {
    gboolean  arrived;
    JsonNode *response;
} Probe;

static void
on_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Probe *probe = user_data;

    probe->response = clawt_mcp_tools_call_finish(CLAWT_MCP_TOOLS(source), result);
    probe->arrived = TRUE;
}

static JsonNode *
parse_request(const gchar *tool, const gchar *arguments, JsonParser *parser)
{
    g_autofree gchar *request = g_strdup_printf(
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"%s\",\"arguments\":%s}}", tool, arguments);

    g_assert_true(json_parser_load_from_data(parser, request, -1, NULL));
    return json_parser_get_root(parser);
}

/* The text of @response, kept for the scan at the end. */
static const gchar *
said(Fixture *fixture, JsonNode *response, gboolean *is_error, guint *n_content)
{
    JsonObject *result = json_object_get_object_member(json_node_get_object(response),
                                                       "result");
    JsonArray *content = json_object_get_array_member(result, "content");
    guint i;

    g_ptr_array_add(fixture->heard, json_to_string(response, FALSE));
    g_test_message("heard: %s", fixture->heard->pdata[fixture->heard->len - 1] != NULL
                   ? (const gchar *)fixture->heard->pdata[fixture->heard->len - 1] : "");
    *is_error = json_object_get_boolean_member_with_default(result, "isError", FALSE);
    if (n_content != NULL)
        *n_content = json_array_get_length(content);
    for (i = 0; i < json_array_get_length(content); i++) {
        JsonObject *item = json_array_get_object_element(content, i);

        if (g_strcmp0(json_object_get_string_member(item, "type"), "text") == 0)
            return json_object_get_string_member(item, "text");
    }
    return "";
}

/* gsurf's and Xvfb's stderr, for a failure's message. */
static void
dump_logs(const gchar *dir)
{
    g_autofree gchar *root = g_build_filename(dir, "browsers", NULL);
    g_autoptr(GDir) sessions = g_dir_open(root, 0, NULL);
    const gchar *session;

    while (sessions != NULL && (session = g_dir_read_name(sessions)) != NULL) {
        static const gchar *const names[] = { "gsurf.log", "xvfb.log", NULL };
        guint i;

        for (i = 0; names[i] != NULL; i++) {
            g_autofree gchar *path = g_build_filename(root, session, names[i], NULL);
            g_autofree gchar *contents = NULL;

            if (g_file_get_contents(path, &contents, NULL, NULL))
                g_printerr("--- %s/%s ---\n%s\n", session, names[i], contents);
        }
    }
}

/* An answer that should (not) be an error, naming what it said if not. */
#define EXPECT_ERROR(want) \
    G_STMT_START { \
        if (is_error != (want)) { \
            dump_logs(fixture.dir); \
            g_error("expected %s, the agent read: %s", \
                    (want) ? "a refusal" : "success", text); \
        } \
    } G_STMT_END

/* One browser tool, on the worker path the daemon uses. */
static JsonNode *
run_tool(Fixture *fixture, const gchar *tool, const gchar *arguments)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    JsonNode *request = parse_request(tool, arguments, parser);
    Probe probe = { FALSE, NULL };
    gint64 deadline;

    g_assert_true(clawt_mcp_tools_call_defers(fixture->tools, "keeper", request));
    clawt_mcp_tools_call_async(fixture->tools, "keeper", "room-1", request, on_done, &probe);

    deadline = g_get_monotonic_time() + TOOL_SECONDS * G_USEC_PER_SEC;
    while (!probe.arrived && g_get_monotonic_time() < deadline)
        g_main_context_iteration(fixture->context, FALSE);
    if (!probe.arrived)
        g_error("%s did not answer within %d seconds", tool, TOOL_SECONDS);
    return probe.response;
}

/* clawtilla_secret_ref, which never touches the browser. */
static gchar *
mint_ref(Fixture *fixture, const gchar *name)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autofree gchar *arguments = g_strdup_printf("{\"name\":\"%s\"}", name);
    g_autoptr(JsonNode) response = clawt_mcp_tools_call(
        fixture->tools, "keeper", "room-1",
        parse_request("clawtilla_secret_ref", arguments, parser));
    gboolean is_error = TRUE;
    const gchar *text = said(fixture, response, &is_error, NULL);
    const gchar *end = strchr(text, '\n');

    g_assert_false(is_error);
    g_assert_true(g_str_has_prefix(text, CLAWT_SECRET_REF_PREFIX));
    return end != NULL ? g_strndup(text, (gsize)(end - text)) : g_strdup(text);
}

static JsonNode *
fill(Fixture *fixture, const gchar *ref, const gchar *selector, gboolean submit)
{
    g_autofree gchar *arguments = g_strdup_printf(
        "{\"secret\":\"%s\",\"selector\":\"%s\",\"submit\":%s}",
        ref, selector, submit ? "true" : "false");

    return run_tool(fixture, "clawtilla_browser_fill_secret", arguments);
}

/* The TOTP codes valid around now: the fill may straddle a step. */
static gboolean
is_current_code(const gchar *code)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtTotpKey) key = clawt_totp_key_parse(TOTP_SEED, &error);
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    gint64 step;

    g_assert_no_error(error);
    for (step = -1; step <= 1; step++) {
        g_autofree gchar *candidate = clawt_totp_code(key, now + step * 30);

        if (g_strcmp0(candidate, code) == 0)
            return TRUE;
    }
    return FALSE;
}

/* ── The test ───────────────────────────────────────────────────────── */

static void
test_a_real_login(void)
{
    Fixture fixture = { 0 };
    Site site = { 0 };
    g_autofree gchar *login = NULL;
    g_autofree gchar *wrong_host = NULL;
    g_autofree gchar *ref = NULL;
    g_autofree gchar *posted = NULL;
    g_autoptr(GHashTable) form = NULL;
    gboolean is_error = TRUE;
    const gchar *text;
    const gchar *code;
    guint n_content = 0;
    guint i;

    if (g_getenv("CLAWT_TEST_INTEGRATION") == NULL ||
        g_strcmp0(g_getenv("CLAWT_TEST_INTEGRATION"), "1") != 0) {
        g_test_skip("set CLAWT_TEST_INTEGRATION=1");
        return;
    }
#ifndef CLAWT_HAVE_GSURF
    g_test_skip("built without GSURF=1");
    return;
#else
    {
        g_autofree gchar *xvfb = g_find_program_in_path("Xvfb");

        if (xvfb == NULL) {
            g_test_skip("Xvfb is not installed");
            return;
        }
    }
#endif

    site_start(&site);
    fixture_setup(&fixture);
    login = g_strdup_printf("http://127.0.0.1:%u/login", site.port);
    wrong_host = g_strdup_printf("http://localhost:%u/login", site.port);

    /* 1. The same form under another name is not the saved site: the
     *    host is the one the real browser reports, and nothing is typed. */
    {
        g_autofree gchar *arguments = g_strdup_printf("{\"url\":\"%s\"}", wrong_host);
        g_autoptr(JsonNode) opened = run_tool(&fixture, "clawtilla_browser_open", arguments);
        g_autoptr(JsonNode) refused = NULL;

        text = said(&fixture, opened, &is_error, NULL);
        EXPECT_ERROR(FALSE);
        g_assert_nonnull(strstr(text, "Live login"));

        ref = mint_ref(&fixture, "site-pass");
        refused = fill(&fixture, ref, "#pw", FALSE);
        text = said(&fixture, refused, &is_error, NULL);
        EXPECT_ERROR(TRUE);
        g_assert_nonnull(strstr(text, "This page is on localhost"));
        g_assert_nonnull(strstr(text, "Nothing was filled"));
        g_clear_pointer(&ref, g_free);
    }

    /* 2. The saved site, and a password typed into it. */
    {
        g_autofree gchar *arguments = g_strdup_printf("{\"url\":\"%s\"}", login);
        g_autoptr(JsonNode) opened = run_tool(&fixture, "clawtilla_browser_open", arguments);
        g_autoptr(JsonNode) filled = NULL;

        text = said(&fixture, opened, &is_error, NULL);
        EXPECT_ERROR(FALSE);
        g_assert_nonnull(strstr(text, "Live login"));

        ref = mint_ref(&fixture, "site-pass");
        filled = fill(&fixture, ref, "#pw", FALSE);
        text = said(&fixture, filled, &is_error, NULL);
        EXPECT_ERROR(FALSE);
        g_assert_true(g_str_has_prefix(text, "Filled site-pass into #pw."));
        g_clear_pointer(&ref, g_free);
    }

    /* 3. The hold: gsurf itself, not only the daemon, refuses a picture
     *    and the page's HTML while a secret sits in the form. */
    {
        g_autoptr(JsonNode) shot = run_tool(&fixture, "clawtilla_browser_screenshot", "{}");
        g_autoptr(JsonNode) html = run_tool(&fixture, "clawtilla_browser_html", "{}");
        g_autoptr(ClawtBrowser) browser =
            clawt_browser_manager_lookup(fixture.browsers, "agent:keeper");
        g_autoptr(GError) error = NULL;
        g_autoptr(JsonNode) direct = NULL;

        text = said(&fixture, shot, &is_error, NULL);
        EXPECT_ERROR(TRUE);
        text = said(&fixture, html, &is_error, NULL);
        EXPECT_ERROR(TRUE);

        /* Past the daemon's own wall, straight to gsurf's socket. */
        g_assert_nonnull(browser);
        {
            g_autoptr(JsonParser) parser = json_parser_new();

            g_assert_true(json_parser_load_from_data(parser, "{\"inline\":true}", -1, NULL));
            direct = clawt_browser_call(browser, "page.screenshot",
                                        json_parser_get_root(parser), 30, &error);
        }
        g_assert_null(direct);
        g_assert_nonnull(error);
        if (strstr(error->message, "secret") == NULL)
            g_error("gsurf refused for another reason: %s", error->message);
    }

    /* 4. A one-time code, and Enter: the form is posted. */
    {
        g_autoptr(JsonNode) filled = NULL;

        ref = mint_ref(&fixture, "site-2fa");
        filled = fill(&fixture, ref, "#otp", TRUE);
        text = said(&fixture, filled, &is_error, NULL);
        EXPECT_ERROR(FALSE);
        g_assert_true(g_str_has_prefix(text, "Filled site-2fa into #otp and pressed Enter."));
        g_assert_nonnull(strstr(text, "Signed in"));
    }

    /* 5. What the site received is what the vault holds, as typed keys. */
    posted = site_dup_posted(&site);
    g_assert_nonnull(posted);
    form = soup_form_decode(posted);
    g_assert_cmpstr(g_hash_table_lookup(form, "pw"), ==, SECRET_VALUE);
    code = g_hash_table_lookup(form, "otp");
    g_assert_nonnull(code);
    g_assert_cmpuint(strlen(code), ==, 6);
    g_assert_true(is_current_code(code));

    /* 6. A new page, so the hold is gone and a picture is allowed again. */
    {
        g_autoptr(JsonNode) shot = run_tool(&fixture, "clawtilla_browser_screenshot", "{}");

        text = said(&fixture, shot, &is_error, &n_content);
        EXPECT_ERROR(FALSE);
        g_assert_cmpuint(n_content, ==, 2);
    }

    /* 7. Nothing the agent read carried the password or the code. */
    for (i = 0; i < fixture.heard->len; i++) {
        const gchar *reply = g_ptr_array_index(fixture.heard, i);

        g_assert_null(strstr(reply, SECRET_VALUE));
        g_assert_null(strstr(reply, TOTP_SEED));
        g_assert_null(strstr(reply, code));
    }

    fixture_teardown(&fixture);
    site_stop(&site);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/browser-live/a-real-login", test_a_real_login);

    return g_test_run();
}
