/*
 * test-browser.c - Browser sessions, without a browser
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Everything above the control socket runs here with a fake transport
 * standing in for gsurf: which session an agent drives, the keys a shared
 * session takes from its `browsers:` entry, the one-driver lease, the
 * tools an agent is offered and what they send.  The real thing -- an
 * Xvfb, a gsurf, a page whose text only JavaScript writes -- is
 * tests/test-browser-integration.sh, behind CLAWT_TEST_INTEGRATION=1.
 */

#include "clawtilla.h"
#include "clawt-test-util.h"

#include <glib/gstdio.h>
#include <string.h>

/* ── A fake gsurf ─────────────────────────────────────────────────── */

typedef struct {
    GPtrArray *methods;     /* every method called, in order */
    GPtrArray *params;      /* their params, as JSON text ("" for none) */
    gboolean   held;        /* gsurf's secret hold, as the fake keeps it */
    gboolean   hold_fails;  /* page.secret_hold answers with an error */
} Fake;

static JsonNode *
string_node(const gchar *text)
{
    JsonNode *node = json_node_new(JSON_NODE_VALUE);

    json_node_set_string(node, text);
    return node;
}

static JsonNode *
fake_transport(ClawtBrowser *browser, const gchar *method, JsonNode *params,
               GError **error, gpointer user_data)
{
    Fake *fake = user_data;

    (void)browser;

    g_ptr_array_add(fake->methods, g_strdup(method));
    g_ptr_array_add(fake->params, params != NULL ? json_to_string(params, FALSE)
                                                 : g_strdup(""));

    /* The secret hold, as gsurf 0.5.13 keeps it: set by a fill (lifted at
     * once by its submit, which navigates), lifted by navigating, and
     * refusing screenshots and scripts in between. */
    if (g_strcmp0(method, "navigate") == 0)
        fake->held = FALSE;
    if (g_strcmp0(method, "page.fill_secret") == 0) {
        JsonNode *node = json_node_new(JSON_NODE_OBJECT);
        JsonObject *object = json_object_new();

        fake->held = !json_object_get_boolean_member_with_default(
            json_node_get_object(params), "submit", FALSE);
        json_object_set_boolean_member(object, "done", TRUE);
        json_object_set_boolean_member(object, "secret_hold", TRUE);
        json_node_take_object(node, object);
        return node;
    }
    if (g_strcmp0(method, "page.secret_hold") == 0) {
        JsonNode *node;
        JsonObject *object;

        if (fake->hold_fails) {
            g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                                "page.secret_hold: the browser did not answer");
            return NULL;
        }
        node = json_node_new(JSON_NODE_OBJECT);
        object = json_object_new();
        json_object_set_boolean_member(object, "secret_hold", fake->held);
        json_node_take_object(node, object);
        return node;
    }
    if (fake->held && (g_strcmp0(method, "page.screenshot") == 0 ||
                       (g_strcmp0(method, "page.eval") == 0 &&
                        g_strcmp0(json_object_get_string_member(json_node_get_object(params),
                                                                "script"),
                                  "document.readyState") != 0))) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED,
                            "refused: a secret was typed into this page");
        return NULL;
    }

    if (g_strcmp0(method, "page.text") == 0)
        return string_node("rendered by javascript");
    if (g_strcmp0(method, "page.uri") == 0)
        return string_node("https://example.org/");
    if (g_strcmp0(method, "page.title") == 0)
        return string_node("Example Domain");
    if (g_strcmp0(method, "page.eval") == 0) {
        const gchar *script = json_object_get_string_member(json_node_get_object(params),
                                                            "script");

        if (g_strcmp0(script, "document.readyState") == 0)
            return string_node("complete");
        if (strstr(script, "querySelectorAll('a[href]')") != NULL)
            return string_node("More information -> https://www.iana.org/domains/example");
        return string_node("evaluated");
    }
    if (g_strcmp0(method, "page.screenshot") == 0) {
        JsonNode *node = json_node_new(JSON_NODE_OBJECT);
        JsonObject *object = json_object_new();

        /* "PNG" is enough: nothing here decodes it */
        json_object_set_string_member(object, "png_base64", "iVBORw==");
        json_node_take_object(node, object);
        return node;
    }

    {
        JsonNode *node = json_node_new(JSON_NODE_OBJECT);

        json_node_take_object(node, json_object_new());
        return node;
    }
}

static Fake *
fake_new(void)
{
    Fake *fake = g_new0(Fake, 1);

    fake->methods = g_ptr_array_new_with_free_func(g_free);
    fake->params = g_ptr_array_new_with_free_func(g_free);
    return fake;
}

static void
fake_free(Fake *fake)
{
    g_ptr_array_unref(fake->methods);
    g_ptr_array_unref(fake->params);
    g_free(fake);
}

/* The params of the first call to @method, or NULL. */
static const gchar *
fake_params_of(Fake *fake, const gchar *method)
{
    guint i;

    for (i = 0; i < fake->methods->len; i++) {
        if (g_strcmp0(g_ptr_array_index(fake->methods, i), method) == 0)
            return g_ptr_array_index(fake->params, i);
    }
    return NULL;
}

/* ── Keys and config ──────────────────────────────────────────────── */

static void
test_session_keys(void)
{
    g_autofree gchar *own = clawt_browser_manager_session_key(CLAWT_BROWSER_OWN, "a", NULL);
    g_autofree gchar *shared = clawt_browser_manager_session_key(CLAWT_BROWSER_SHARED, "a",
                                                                 "web");
    g_autofree gchar *none = clawt_browser_manager_session_key(CLAWT_BROWSER_NONE, "a", "web");
    g_autofree gchar *unnamed = clawt_browser_manager_session_key(CLAWT_BROWSER_SHARED, "a",
                                                                  "");

    g_assert_cmpstr(own, ==, "agent:a");
    g_assert_cmpstr(shared, ==, "shared:web");
    g_assert_null(none);
    g_assert_null(unnamed);
}

static ClawtConfig *
load(const gchar *yaml)
{
    g_autoptr(GError) error = NULL;
    ClawtConfig *config = clawt_config_load_from_string(yaml, &error);

    g_assert_no_error(error);
    return config;
}

/*
 * A shared session's keys come from its browsers: entry; mode and use
 * stay the agent's own.  Read through the same getters every backend and
 * client uses, so none of them has to know there is a shared session.
 */
static void
test_shared_keys_come_from_the_entry(void)
{
    g_autoptr(ClawtConfig) config = load(
        "browsers:\n"
        "  - id: web\n"
        "    home: https://start.example\n"
        "    width: 900\n"
        "agents:\n"
        "  - id: a\n"
        "    browser:\n"
        "      mode: shared\n"
        "      use: web\n"
        "      home: https://ignored.example\n"
        "  - id: b\n"
        "    browser:\n"
        "      mode: own\n"
        "      home: https://own.example\n");
    ClawtAgentConfig *a = clawt_config_get_agent(config, "a");
    ClawtAgentConfig *b = clawt_config_get_agent(config, "b");
    g_autoptr(GPtrArray) ids = clawt_config_get_browser_ids(config);

    g_assert_cmpint(clawt_agent_config_get_enum(a, "browser.mode"), ==, CLAWT_BROWSER_SHARED);
    g_assert_cmpstr(clawt_agent_config_get_string(a, "browser.use"), ==, "web");
    g_assert_cmpstr(clawt_agent_config_get_string(a, "browser.home"), ==,
                    "https://start.example");
    g_assert_cmpint(clawt_agent_config_get_int(a, "browser.width"), ==, 900);
    /* not in the entry: the schema default, not the agent's own block */
    g_assert_cmpint(clawt_agent_config_get_int(a, "browser.idle_minutes"), ==, 15);

    g_assert_cmpint(clawt_agent_config_get_enum(b, "browser.mode"), ==, CLAWT_BROWSER_OWN);
    g_assert_cmpstr(clawt_agent_config_get_string(b, "browser.home"), ==,
                    "https://own.example");

    g_assert_cmpuint(ids->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(ids, 0), ==, "web");
}

/*
 * An agent asking for a browser this build cannot start, or a shared one
 * that is not declared, is disabled with the reason -- never run without
 * the tools it was told it has.
 */
static void
test_a_browser_that_cannot_be_is_a_shadow(void)
{
    g_autoptr(ClawtConfig) config = load(
        "agents:\n"
        "  - id: plain\n"
        "  - id: own\n"
        "    browser:\n"
        "      mode: own\n"
        "  - id: lost\n"
        "    browser:\n"
        "      mode: shared\n"
        "      use: nowhere\n");
    const gchar *plain = clawt_agent_config_get_shadow_reason(
        clawt_config_get_agent(config, "plain"));
    const gchar *own = clawt_agent_config_get_shadow_reason(
        clawt_config_get_agent(config, "own"));
    const gchar *lost = clawt_agent_config_get_shadow_reason(
        clawt_config_get_agent(config, "lost"));

    g_assert_null(plain);

    if (clawt_browser_build_available()) {
        g_assert_null(own);
        g_assert_nonnull(lost);
        g_assert_nonnull(strstr(lost, "no browser 'nowhere'"));
    } else {
        g_assert_nonnull(own);
        g_assert_nonnull(strstr(own, "GSURF=1"));
        g_assert_nonnull(lost);
        g_assert_nonnull(strstr(lost, "GSURF=1"));
    }
}

/* ── The session ──────────────────────────────────────────────────── */

/* One driver at a time; the claim names the holder and lapses on its own. */
static void
test_one_driver_at_a_time(void)
{
    g_autoptr(ClawtBrowser) browser = clawt_browser_new("web", "/nonexistent/web");
    g_autoptr(GError) error = NULL;
    g_autofree gchar *driver = NULL;
    g_autofree gchar *lapsed = NULL;
    gint64 now = 1000000;

    g_assert_true(clawt_browser_claim(browser, "alpha", now, NULL));
    g_assert_true(clawt_browser_claim(browser, "alpha", now + 5, NULL));
    g_assert_false(clawt_browser_claim(browser, "beta", now + 10, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED);
    g_assert_nonnull(strstr(error->message, "alpha is driving"));
    driver = clawt_browser_dup_driver(browser, now + 10);
    g_assert_cmpstr(driver, ==, "alpha");

    /* releasing somebody else's claim does nothing */
    g_assert_false(clawt_browser_release(browser, "beta"));

    /* lapsed: measured from alpha's last call, not its first */
    lapsed = clawt_browser_dup_driver(browser,
                                      now + 5 + CLAWT_BROWSER_DRIVER_LEASE_SECONDS);
    g_assert_null(lapsed);
    g_assert_true(clawt_browser_claim(browser, "beta",
                                      now + 5 + CLAWT_BROWSER_DRIVER_LEASE_SECONDS, NULL));
    g_assert_true(clawt_browser_release(browser, "beta"));
    g_assert_true(clawt_browser_claim(browser, "alpha", now + 400, NULL));
}

/* An own session and a shared one with the same id never share a profile. */
static void
test_sessions_keep_apart(void)
{
    g_autoptr(ClawtBrowserManager) manager = clawt_browser_manager_new("/nonexistent/b");
    g_autoptr(ClawtBrowser) own = clawt_browser_manager_obtain(manager, "agent:web");
    g_autoptr(ClawtBrowser) shared = clawt_browser_manager_obtain(manager, "shared:web");
    g_autoptr(ClawtBrowser) again = clawt_browser_manager_obtain(manager, "shared:web");
    g_autoptr(ClawtBrowser) missing = clawt_browser_manager_lookup(manager, "agent:nobody");
    g_autoptr(GPtrArray) all = clawt_browser_manager_list(manager);

    g_assert_true(shared == again);
    g_assert_true(own != shared);
    g_assert_cmpstr(clawt_browser_get_id(own), ==, "web");
    g_assert_cmpstr(clawt_browser_get_dir(own), ==, "/nonexistent/b/agent.web");
    g_assert_cmpstr(clawt_browser_get_dir(shared), ==, "/nonexistent/b/shared.web");
    g_assert_null(missing);
    g_assert_cmpuint(all->len, ==, 2);
    g_assert_false(clawt_browser_is_running(own));
}

/* The line every client draws, from the listing every client reads. */
static void
test_the_session_line(void)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    JsonObject *reply;
    JsonObject *session;
    g_autofree gchar *running = NULL;
    g_autofree gchar *stopped = NULL;

    g_assert_true(json_parser_load_from_data(parser,
        "{\"sessions\":["
        " {\"key\":\"shared:web\",\"agents\":[\"a\",\"b\"],\"running\":true,"
        "  \"uri\":\"https://example.org/\",\"title\":\"Example\",\"driver\":\"b\"},"
        " {\"key\":\"agent:c\",\"agents\":[\"c\"],\"running\":false}]}", -1, NULL));
    reply = json_node_get_object(json_parser_get_root(parser));

    session = clawt_browser_list_find(reply, "b");
    g_assert_nonnull(session);
    running = clawt_browser_session_line(session);
    g_assert_cmpstr(running, ==, "Running \xe2\x80\x94 Example (https://example.org/) "
                                 "\xe2\x80\x94 driven by b");
    stopped = clawt_browser_session_line(clawt_browser_list_find(reply, "c"));
    g_assert_true(g_str_has_prefix(stopped, "Not running."));
    g_assert_null(clawt_browser_list_find(reply, "nobody"));
}

/* ── The tools ────────────────────────────────────────────────────── */

typedef struct {
    gchar               *dir;
    ClawtConfig         *config;
    ClawtAgentManager   *agents;
    ClawtTaskManager    *tasks;
    ClawtLoopGuard      *guard;
    ClawtMcpTools       *tools;
    ClawtBrowserManager *browsers;
    GMainContext        *context;
    Fake                *fake;
} Fixture;

#define SECRET_VALUE "hunter2-zz-never-shown"
/* "12345678901234567890" in base32: RFC 6238's SHA1 seed. */
#define TOTP_SEED "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ"

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

    fixture->dir = g_dir_make_tmp("clawt-browser-XXXXXX", NULL);
    write_secret(fixture->dir, "site-pass", SECRET_VALUE);
    write_secret(fixture->dir, "site-2fa", TOTP_SEED);
    yaml = g_strdup_printf("daemon:\n  state_dir: \"%s\"\n"
                           "defaults:\n  workspace_root: \"%s/agents\"\n"
                           "secrets:\n  dir: \"%s\"\n"
                           "browsers:\n  - id: web\n"
                           "vault:\n"
                           "  - name: site-pass\n"
                           "    secret: {file: site-pass}\n"
                           "    sites: [example.org]\n"
                           "  - name: site-2fa\n"
                           "    kind: totp\n"
                           "    secret: {file: site-2fa}\n"
                           "    sites: [example.org]\n"
                           "  - name: elsewhere\n"
                           "    secret: {file: site-pass}\n"
                           "    sites: [github.com]\n"
                           "agents:\n"
                           "  - id: keeper\n"
                           "    browser:\n      mode: own\n"
                           "      secrets: [site-pass, site-2fa, elsewhere]\n"
                           "  - id: surfer\n"
                           "    browser:\n      mode: own\n"
                           "  - id: scripter\n"
                           "    browser:\n      mode: own\n      allow_scripts: true\n"
                           "  - id: one\n"
                           "    browser:\n      mode: shared\n      use: web\n"
                           "  - id: two\n"
                           "    browser:\n      mode: shared\n      use: web\n"
                           "  - id: plain\n",
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

    root = g_build_filename(fixture->dir, "browsers", NULL);
    fixture->browsers = clawt_browser_manager_new(root);
    fixture->fake = fake_new();
    clawt_browser_manager_set_transport(fixture->browsers, fake_transport, fixture->fake);
    clawt_mcp_tools_set_browsers(fixture->tools, fixture->browsers);
}

static void
fixture_teardown(Fixture *fixture)
{
    g_clear_object(&fixture->tools);
    g_clear_object(&fixture->browsers);
    g_clear_object(&fixture->guard);
    g_clear_object(&fixture->tasks);
    g_clear_object(&fixture->agents);
    g_clear_object(&fixture->config);
    g_clear_pointer(&fixture->context, g_main_context_unref);
    fake_free(fixture->fake);
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

static JsonNode *run_tool_in(Fixture *fixture, const gchar *agent_id, const gchar *room_id,
                             const gchar *tool, const gchar *arguments);

/* One browser tool through the deferred path, with a watchdog. */
static JsonNode *
run_tool(Fixture *fixture, const gchar *agent_id, const gchar *tool,
         const gchar *arguments)
{
    return run_tool_in(fixture, agent_id, NULL, tool, arguments);
}

/* The same, from a named conversation. */
static JsonNode *
run_tool_in(Fixture *fixture, const gchar *agent_id, const gchar *room_id,
            const gchar *tool, const gchar *arguments)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autofree gchar *request = g_strdup_printf(
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"%s\",\"arguments\":%s}}", tool, arguments);
    Probe probe = { FALSE, NULL };
    gint64 deadline;

    g_assert_true(json_parser_load_from_data(parser, request, -1, NULL));
    g_assert_true(clawt_mcp_tools_call_defers(fixture->tools, agent_id,
                                              json_parser_get_root(parser)));
    clawt_mcp_tools_call_async(fixture->tools, agent_id, room_id,
                               json_parser_get_root(parser), on_done, &probe);

    deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
    while (!probe.arrived && g_get_monotonic_time() < deadline)
        g_main_context_iteration(fixture->context, FALSE);
    g_assert_true(probe.arrived);
    return probe.response;
}

static const gchar *
result_text(JsonNode *response, gboolean *is_error, guint *n_content)
{
    JsonObject *result = json_object_get_object_member(json_node_get_object(response),
                                                       "result");
    JsonArray *content = json_object_get_array_member(result, "content");
    guint i;

    *is_error = json_object_get_boolean_member_with_default(result, "isError", FALSE);
    if (n_content != NULL)
        *n_content = json_array_get_length(content);
    for (i = 0; i < json_array_get_length(content); i++) {
        JsonObject *item = json_array_get_object_element(content, i);

        if (g_strcmp0(json_object_get_string_member(item, "type"), "text") == 0)
            return json_object_get_string_member(item, "text");
    }
    return NULL;
}

/* Offered where there is a browser; eval only with allow_scripts. */
static void
test_tools_follow_the_browser(void)
{
    Fixture fixture = { 0 };

    fixture_setup(&fixture);

    g_assert_true(clawt_mcp_tools_is_permitted(fixture.tools, "surfer",
                                               "clawtilla_browser_read"));
    g_assert_false(clawt_mcp_tools_is_permitted(fixture.tools, "surfer",
                                                "clawtilla_browser_eval"));
    g_assert_true(clawt_mcp_tools_is_permitted(fixture.tools, "scripter",
                                               "clawtilla_browser_eval"));
    g_assert_false(clawt_mcp_tools_is_permitted(fixture.tools, "plain",
                                                "clawtilla_browser_open"));

    /* and nobody, from a daemon that holds no sessions */
    clawt_mcp_tools_set_browsers(fixture.tools, NULL);
    g_assert_false(clawt_mcp_tools_is_permitted(fixture.tools, "surfer",
                                                "clawtilla_browser_read"));

    fixture_teardown(&fixture);
}

/* What a tool sends, and what the agent reads back. */
static void
test_tools_send_and_say(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) opened = NULL;
    g_autoptr(JsonNode) read = NULL;
    g_autoptr(JsonNode) clicked = NULL;
    g_autoptr(JsonNode) typed = NULL;
    g_autoptr(JsonNode) shot = NULL;
    gboolean is_error = TRUE;
    const gchar *text;
    guint n_content = 0;

    fixture_setup(&fixture);

    opened = run_tool(&fixture, "surfer", "clawtilla_browser_open", "{\"url\":\"example.org\"}");
    text = result_text(opened, &is_error, NULL);
    g_assert_false(is_error);
    g_assert_cmpstr(fake_params_of(fixture.fake, "navigate"), ==,
                    "{\"uri\":\"https://example.org\"}");
    g_assert_nonnull(strstr(text, "Now on https://example.org/ -- \"Example Domain\""));

    read = run_tool(&fixture, "surfer", "clawtilla_browser_read", "{}");
    text = result_text(read, &is_error, NULL);
    g_assert_false(is_error);
    g_assert_nonnull(strstr(text, "rendered by javascript"));
    g_assert_nonnull(strstr(text, "Links:\nMore information -> https://www.iana.org/"));

    clicked = run_tool(&fixture, "surfer", "clawtilla_browser_click",
                       "{\"selector\":\"#go\"}");
    text = result_text(clicked, &is_error, NULL);
    g_assert_false(is_error);
    g_assert_cmpstr(fake_params_of(fixture.fake, "page.click"), ==,
                    "{\"selector\":\"#go\",\"button\":1}");

    typed = run_tool(&fixture, "surfer", "clawtilla_browser_type",
                     "{\"text\":\"hello\",\"selector\":\"#q\",\"submit\":true}");
    text = result_text(typed, &is_error, NULL);
    g_assert_false(is_error);
    g_assert_cmpstr(fake_params_of(fixture.fake, "page.type"), ==,
                    "{\"text\":\"hello\",\"selector\":\"#q\"}");
    g_assert_cmpstr(fake_params_of(fixture.fake, "page.key"), ==, "{\"key\":\"Enter\"}");
    g_assert_nonnull(strstr(text, "Typed 5 character(s) and pressed Enter"));

    shot = run_tool(&fixture, "surfer", "clawtilla_browser_screenshot", "{}");
    text = result_text(shot, &is_error, &n_content);
    g_assert_false(is_error);
    g_assert_cmpuint(n_content, ==, 2);

    fixture_teardown(&fixture);
}

/* Two agents on one shared session: the second is refused while the first drives. */
static void
test_a_shared_session_has_one_driver(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) first = NULL;
    g_autoptr(JsonNode) refused = NULL;
    g_autoptr(JsonNode) released = NULL;
    g_autoptr(JsonNode) second = NULL;
    gboolean is_error = FALSE;
    const gchar *text;

    fixture_setup(&fixture);

    first = run_tool(&fixture, "one", "clawtilla_browser_read", "{}");
    result_text(first, &is_error, NULL);
    g_assert_false(is_error);

    refused = run_tool(&fixture, "two", "clawtilla_browser_read", "{}");
    text = result_text(refused, &is_error, NULL);
    g_assert_true(is_error);
    g_assert_nonnull(strstr(text, "one is driving the shared browser 'web'"));

    released = run_tool(&fixture, "one", "clawtilla_browser_release", "{}");
    text = result_text(released, &is_error, NULL);
    g_assert_false(is_error);
    g_assert_true(g_str_has_prefix(text, "Released."));

    second = run_tool(&fixture, "two", "clawtilla_browser_read", "{}");
    result_text(second, &is_error, NULL);
    g_assert_false(is_error);

    fixture_teardown(&fixture);
}

/* eval runs, and answers in JSON, where allow_scripts grants it. */
static void
test_eval_runs_with_allow_scripts(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) allowed = NULL;
    gboolean is_error = TRUE;
    const gchar *text;

    fixture_setup(&fixture);

    allowed = run_tool(&fixture, "scripter", "clawtilla_browser_eval",
                       "{\"script\":\"1 + 1\"}");
    text = result_text(allowed, &is_error, NULL);
    g_assert_false(is_error);
    g_assert_cmpstr(text, ==, "\"evaluated\"");

    fixture_teardown(&fixture);
}

/* ── Secrets the agent never sees ─────────────────────────────────── */

/* Everything the daemon logged while the test ran (the writer is set in main). */
static GString *log_capture;

static GLogWriterOutput
capture_writer(GLogLevelFlags level, const GLogField *fields, gsize n_fields,
               gpointer user_data)
{
    gsize i;

    for (i = 0; i < n_fields; i++) {
        if (g_strcmp0(fields[i].key, "MESSAGE") == 0 && fields[i].length < 0)
            g_string_append_printf(log_capture, "%s\n", (const gchar *)fields[i].value);
    }
    return g_log_writer_default(level, fields, n_fields, user_data);
}

static void
on_bus_event(ClawtEventBus *bus, ClawtEvent *event, gpointer user_data)
{
    GPtrArray *events = user_data;
    g_autoptr(JsonNode) node = clawt_event_to_json(event);

    (void)bus;
    g_ptr_array_add(events, json_to_string(node, FALSE));
}

/* A tool on the synchronous path, from @room_id. */
static JsonNode *
call_sync(Fixture *fixture, const gchar *agent_id, const gchar *room_id, const gchar *tool,
          const gchar *arguments)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autofree gchar *request = g_strdup_printf(
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"%s\",\"arguments\":%s}}", tool, arguments);

    g_assert_true(json_parser_load_from_data(parser, request, -1, NULL));
    return clawt_mcp_tools_call(fixture->tools, agent_id, room_id,
                                json_parser_get_root(parser));
}

/* How many times @method was called. */
static guint
fake_count(Fake *fake, const gchar *method)
{
    guint count = 0;
    guint i;

    for (i = 0; i < fake->methods->len; i++) {
        if (g_strcmp0(g_ptr_array_index(fake->methods, i), method) == 0)
            count++;
    }
    return count;
}

/* The params of the last call to @method, or NULL. */
static const gchar *
fake_last_params_of(Fake *fake, const gchar *method)
{
    guint i;

    for (i = fake->methods->len; i > 0; i--) {
        if (g_strcmp0(g_ptr_array_index(fake->methods, i - 1), method) == 0)
            return g_ptr_array_index(fake->params, i - 1);
    }
    return NULL;
}

/* The reference on the first line of a clawtilla_secret_ref answer. */
static gchar *
ref_of(JsonNode *response)
{
    gboolean is_error = TRUE;
    const gchar *text = result_text(response, &is_error, NULL);
    const gchar *end;

    g_assert_false(is_error);
    g_assert_true(g_str_has_prefix(text, CLAWT_SECRET_REF_PREFIX));
    end = strchr(text, '\n');
    return end != NULL ? g_strndup(text, (gsize)(end - text)) : g_strdup(text);
}

/* Keeps @response's text for the negative scan, and answers it. */
static const gchar *
said(GPtrArray *heard, JsonNode *response, gboolean *is_error)
{
    g_ptr_array_add(heard, json_to_string(response, FALSE));
    return result_text(response, is_error, NULL);
}

/*
 * The whole path: a reference, a fill typed by the browser, the hold on
 * screenshots and scripts, the reference spent, its binding to the
 * conversation, the sites, a TOTP code made at the moment of the fill,
 * and the hold failing closed -- with the value, the seed and the code
 * nowhere in what the agent heard, the events, or the log.
 */
static void
test_secrets_never_seen(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtEventBus) bus = clawt_event_bus_new(64);
    g_autoptr(GPtrArray) events = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) heard = g_ptr_array_new_with_free_func(g_free);
    g_autofree gchar *ref = NULL;
    g_autofree gchar *expected = NULL;
    gboolean is_error = FALSE;
    const gchar *text;
    guint i;

    fixture_setup(&fixture);
    g_string_truncate(log_capture, 0);
    clawt_mcp_tools_set_event_bus(fixture.tools, bus);
    g_signal_connect(bus, "event", G_CALLBACK(on_bus_event), events);

    /* 0. Offered to the agent granted entries, and nobody else. */
    g_assert_true(clawt_mcp_tools_is_permitted(fixture.tools, "keeper", "clawtilla_secret_ref"));
    g_assert_true(clawt_mcp_tools_is_permitted(fixture.tools, "keeper",
                                               "clawtilla_browser_fill_secret"));
    g_assert_false(clawt_mcp_tools_is_permitted(fixture.tools, "surfer",
                                                "clawtilla_secret_ref"));
    g_assert_false(clawt_mcp_tools_is_permitted(fixture.tools, "surfer",
                                                "clawtilla_browser_fill_secret"));

    /* 1. The list, and a reference: names only. */
    {
        g_autoptr(JsonNode) listing = call_sync(&fixture, "keeper", "room-1",
                                                "clawtilla_secret_ref", "{}");
        g_autoptr(JsonNode) refused = call_sync(&fixture, "keeper", "room-1",
                                                "clawtilla_secret_ref", "{\"name\":\"nope\"}");
        g_autoptr(JsonNode) minted = call_sync(&fixture, "keeper", "room-1",
                                               "clawtilla_secret_ref",
                                               "{\"name\":\"site-pass\"}");

        text = said(heard, listing, &is_error);
        g_assert_false(is_error);
        g_assert_nonnull(strstr(text, "- site-pass: a password on example.org"));
        g_assert_nonnull(strstr(text, "- site-2fa: a one-time code on example.org"));
        text = said(heard, refused, &is_error);
        g_assert_true(is_error);
        g_assert_nonnull(strstr(text, "'nope' is not a secret you may ask for"));
        g_ptr_array_add(heard, json_to_string(minted, FALSE));
        ref = ref_of(minted);
    }

    /* 2. The fill: gsurf gets the value, the agent hears "filled". */
    {
        g_autofree gchar *arguments = g_strdup_printf(
            "{\"secret\":\"%s\",\"selector\":\"#pw\"}", ref);
        g_autoptr(JsonNode) filled = run_tool_in(&fixture, "keeper", "room-1",
                                                 "clawtilla_browser_fill_secret", arguments);
        g_autoptr(JsonNode) again = NULL;

        text = said(heard, filled, &is_error);
        g_assert_false(is_error);
        g_assert_true(g_str_has_prefix(text, "Filled site-pass into #pw."));
        g_assert_cmpstr(fake_params_of(fixture.fake, "page.fill_secret"), ==,
                        "{\"selector\":\"#pw\",\"text\":\"" SECRET_VALUE "\","
                        "\"submit\":false,\"host\":\"example.org\"}");

        /* ...once */
        again = run_tool_in(&fixture, "keeper", "room-1", "clawtilla_browser_fill_secret",
                            arguments);
        text = said(heard, again, &is_error);
        g_assert_true(is_error);
        g_assert_nonnull(strstr(text, "already used"));
        g_assert_cmpuint(fake_count(fixture.fake, "page.fill_secret"), ==, 1);
    }

    /* 3. The hold: no picture, no HTML, until the page changes. */
    {
        g_autoptr(JsonNode) shot = run_tool(&fixture, "keeper", "clawtilla_browser_screenshot",
                                            "{}");
        g_autoptr(JsonNode) html = run_tool(&fixture, "keeper", "clawtilla_browser_html", "{}");
        g_autoptr(JsonNode) read = run_tool(&fixture, "keeper", "clawtilla_browser_read", "{}");
        g_autoptr(JsonNode) opened = NULL;
        g_autoptr(JsonNode) after = NULL;
        guint n_content = 0;

        text = said(heard, shot, &is_error);
        g_assert_true(is_error);
        g_assert_nonnull(strstr(text, "a secret was just filled on this page"));
        text = said(heard, html, &is_error);
        g_assert_true(is_error);
        g_assert_cmpuint(fake_count(fixture.fake, "page.screenshot"), ==, 0);
        /* reading the text is not reading the field */
        said(heard, read, &is_error);
        g_assert_false(is_error);

        opened = run_tool(&fixture, "keeper", "clawtilla_browser_open",
                          "{\"url\":\"example.org\"}");
        said(heard, opened, &is_error);
        g_assert_false(is_error);
        after = run_tool(&fixture, "keeper", "clawtilla_browser_screenshot", "{}");
        g_ptr_array_add(heard, g_strdup(""));
        result_text(after, &is_error, &n_content);
        g_assert_false(is_error);
        g_assert_cmpuint(n_content, ==, 2);
    }

    /* 4. Bound to the conversation; and only on the entry's sites. */
    {
        g_autoptr(JsonNode) minted = call_sync(&fixture, "keeper", "room-1",
                                               "clawtilla_secret_ref",
                                               "{\"name\":\"site-pass\"}");
        g_autoptr(JsonNode) away = call_sync(&fixture, "keeper", "room-1",
                                             "clawtilla_secret_ref",
                                             "{\"name\":\"elsewhere\"}");
        g_autofree gchar *moved = ref_of(minted);
        g_autofree gchar *elsewhere = ref_of(away);
        g_autofree gchar *moved_arguments = g_strdup_printf(
            "{\"secret\":\"%s\",\"selector\":\"#pw\"}", moved);
        g_autofree gchar *away_arguments = g_strdup_printf(
            "{\"secret\":\"%s\",\"selector\":\"#pw\"}", elsewhere);
        g_autoptr(JsonNode) wrong_room = run_tool_in(&fixture, "keeper", "room-2",
                                                     "clawtilla_browser_fill_secret",
                                                     moved_arguments);
        g_autoptr(JsonNode) wrong_site = run_tool_in(&fixture, "keeper", "room-1",
                                                     "clawtilla_browser_fill_secret",
                                                     away_arguments);

        text = said(heard, wrong_room, &is_error);
        g_assert_true(is_error);
        g_assert_nonnull(strstr(text, "another conversation"));
        text = said(heard, wrong_site, &is_error);
        g_assert_true(is_error);
        g_assert_nonnull(strstr(text, "This page is on example.org, and 'elsewhere' is "
                                      "saved for github.com only"));
        g_assert_cmpuint(fake_count(fixture.fake, "page.fill_secret"), ==, 1);
    }

    /* 5. TOTP: the code for now, made from the seed, typed the same way. */
    {
        g_autoptr(JsonNode) minted = call_sync(&fixture, "keeper", "room-1",
                                               "clawtilla_secret_ref",
                                               "{\"name\":\"site-2fa\"}");
        g_autofree gchar *code_ref = ref_of(minted);
        g_autofree gchar *arguments = g_strdup_printf(
            "{\"secret\":\"%s\",\"selector\":\"#otp\"}", code_ref);
        g_autoptr(ClawtTotpKey) key = clawt_totp_key_parse(TOTP_SEED, NULL);
        g_autofree gchar *before = clawt_totp_code(key, g_get_real_time() / G_USEC_PER_SEC);
        g_autoptr(JsonNode) filled = run_tool_in(&fixture, "keeper", "room-1",
                                                 "clawtilla_browser_fill_secret", arguments);
        g_autofree gchar *after = clawt_totp_code(key, g_get_real_time() / G_USEC_PER_SEC);
        g_autofree gchar *sent_before = g_strdup_printf(
            "{\"selector\":\"#otp\",\"text\":\"%s\",\"submit\":false,"
            "\"host\":\"example.org\"}", before);
        g_autofree gchar *sent_after = g_strdup_printf(
            "{\"selector\":\"#otp\",\"text\":\"%s\",\"submit\":false,"
            "\"host\":\"example.org\"}", after);
        const gchar *sent;

        text = said(heard, filled, &is_error);
        g_assert_false(is_error);
        sent = fake_last_params_of(fixture.fake, "page.fill_secret");
        g_assert_true(g_strcmp0(sent, sent_before) == 0 || g_strcmp0(sent, sent_after) == 0);
        g_assert_null(strstr(text, before));
        g_assert_null(strstr(text, after));
        expected = g_strdup(before);
    }

    /* 6. Fail closed: a hold gsurf cannot be asked about stays on. */
    {
        g_autoptr(JsonNode) opened = NULL;
        g_autoptr(JsonNode) shot = NULL;

        fixture.fake->hold_fails = TRUE;
        opened = run_tool(&fixture, "keeper", "clawtilla_browser_open",
                          "{\"url\":\"example.org\"}");
        said(heard, opened, &is_error);
        g_assert_false(fixture.fake->held);   /* gsurf's own hold is gone... */
        shot = run_tool(&fixture, "keeper", "clawtilla_browser_screenshot", "{}");
        text = said(heard, shot, &is_error);
        g_assert_true(is_error);              /* ...and ours stays on without its word */
        g_assert_nonnull(strstr(text, "a secret was just filled"));
        fixture.fake->hold_fails = FALSE;
    }

    /* 7. The event names the entry and the host. */
    while (g_main_context_iteration(fixture.context, FALSE))
        ;
    {
        guint filled = 0;

        for (i = 0; i < events->len; i++) {
            const gchar *line = g_ptr_array_index(events, i);

            if (strstr(line, "browser.secret_filled") != NULL) {
                filled++;
                g_assert_nonnull(strstr(line, "example.org"));
            }
        }
        g_assert_cmpuint(filled, ==, 2);
    }

    /* 8. And the negative half: never the value, the seed or the code. */
    for (i = 0; i < heard->len; i++) {
        const gchar *line = g_ptr_array_index(heard, i);

        g_assert_null(strstr(line, SECRET_VALUE));
        g_assert_null(strstr(line, TOTP_SEED));
    }
    for (i = 0; i < events->len; i++) {
        const gchar *line = g_ptr_array_index(events, i);

        g_assert_null(strstr(line, SECRET_VALUE));
        g_assert_null(strstr(line, TOTP_SEED));
        g_assert_null(strstr(line, expected));
    }
    g_assert_null(strstr(log_capture->str, SECRET_VALUE));
    g_assert_null(strstr(log_capture->str, TOTP_SEED));

    clawt_mcp_tools_set_event_bus(fixture.tools, NULL);
    fixture_teardown(&fixture);
}

/* ── The environment a session is given ──────────────────────────── */

/*
 * A name in the extras replaces the allowlisted copy.  Both used to be
 * written and getenv() finds the first, so a browser's private
 * XDG_DATA_HOME -- or an agent's own env: block -- was silently the
 * daemon's value.
 */
static void
test_extras_replace_the_allowlist(void)
{
    g_autoptr(GHashTable) extra = g_hash_table_new(g_str_hash, g_str_equal);
    g_auto(GStrv) environment = NULL;
    guint seen = 0;
    guint i;

    g_setenv("XDG_DATA_HOME", "/daemon/data", TRUE);
    g_hash_table_insert(extra, (gpointer)"XDG_DATA_HOME", (gpointer)"/session/data");
    environment = clawt_build_child_environment(extra);

    for (i = 0; environment[i] != NULL; i++) {
        if (g_str_has_prefix(environment[i], "XDG_DATA_HOME=")) {
            g_assert_cmpstr(environment[i], ==, "XDG_DATA_HOME=/session/data");
            seen++;
        }
    }
    g_assert_cmpuint(seen, ==, 1);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);
    log_capture = g_string_new(NULL);
    g_log_set_writer_func(capture_writer, NULL, NULL);

    g_test_add_func("/browser/session-keys", test_session_keys);
    g_test_add_func("/browser/config/shared-keys-come-from-the-entry",
                    test_shared_keys_come_from_the_entry);
    g_test_add_func("/browser/config/a-browser-that-cannot-be-is-a-shadow",
                    test_a_browser_that_cannot_be_is_a_shadow);
    g_test_add_func("/browser/one-driver-at-a-time", test_one_driver_at_a_time);
    g_test_add_func("/browser/sessions-keep-apart", test_sessions_keep_apart);
    g_test_add_func("/browser/the-session-line", test_the_session_line);
    g_test_add_func("/browser/tools/follow-the-browser", test_tools_follow_the_browser);
    g_test_add_func("/browser/tools/send-and-say", test_tools_send_and_say);
    g_test_add_func("/browser/tools/a-shared-session-has-one-driver",
                    test_a_shared_session_has_one_driver);
    g_test_add_func("/browser/tools/eval-with-allow-scripts",
                    test_eval_runs_with_allow_scripts);
    g_test_add_func("/browser/tools/secrets-never-seen", test_secrets_never_seen);
    g_test_add_func("/browser/env/extras-replace-the-allowlist",
                    test_extras_replace_the_allowlist);

    return g_test_run();
}
