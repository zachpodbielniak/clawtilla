/*
 * test-web-research.c - The libresearch integration, tools and skill
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Covers the three ways an agent comes to use libresearch, and the one
 * way it reaches it: the clawtilla_web_* tools are offered exactly when
 * the search hook answers for the agent, run on the deferred path (a
 * research run takes minutes), and answer with the server's own words;
 * the web-research skill is installed into skills.dir, replaced only when
 * it is clawtilla's own copy, and assigned with the integration; and an
 * agent whose CLI loads no skills (codex) gets the reference inline.
 *
 * The HTTP side is a SoupServer on loopback serving canned documents, the
 * same harness test-oauth uses; ai-glib's own suite covers the wire.
 */

#include "clawtilla.h"
#include "clawt-test-util.h"

#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <string.h>

/* ── A canned libresearch ─────────────────────────────────────────── */

typedef struct {
    SoupServer *server;
    gchar      *url;
    gchar      *last_path;
    gchar      *last_query;
} FakeServer;

static const gchar QUERY_DOC[] =
    "{\"schema\":\"libresearch.query/1\",\"query\":\"search \\\"gtk\\\" | fields url, title\","
    "\"columns\":[{\"name\":\"url\",\"type\":\"String\"},{\"name\":\"title\",\"type\":\"String\"}],"
    "\"rows\":[{\"url\":\"https://docs.gtk.org/\",\"title\":\"GTK Docs\"}],"
    "\"engines\":[],\"confidential\":false,\"explain\":null,\"elapsed_ms\":3}";

static void
on_request(SoupServer *server, SoupServerMessage *msg, const char *path,
           GHashTable *query, gpointer user_data)
{
    FakeServer *fake = user_data;
    GUri *uri = soup_server_message_get_uri(msg);

    (void)server;
    (void)query;
    g_free(fake->last_path);
    fake->last_path = g_strdup(path);
    g_free(fake->last_query);
    fake->last_query = g_strdup(g_uri_get_query(uri));

    if (g_str_has_prefix(path, "/api/v1/query")) {
        soup_server_message_set_status(msg, 200, NULL);
        soup_server_message_set_response(msg, "application/json", SOUP_MEMORY_STATIC,
                                         QUERY_DOC, strlen(QUERY_DOC));
        return;
    }

    {
        static const gchar envelope[] =
            "{\"error\":{\"code\":\"not_found\",\"message\":\"the research module is not active\"}}";

        soup_server_message_set_status(msg, 404, NULL);
        soup_server_message_set_response(msg, "application/json", SOUP_MEMORY_STATIC,
                                         envelope, strlen(envelope));
    }
}

static FakeServer *
fake_server_start(void)
{
    FakeServer *fake = g_new0(FakeServer, 1);
    g_autoptr(GError) error = NULL;
    GSList *uris;

    fake->server = soup_server_new(NULL, NULL);
    soup_server_add_handler(fake->server, NULL, on_request, fake, NULL);
    g_assert_true(soup_server_listen_local(fake->server, 0, 0, &error));
    g_assert_no_error(error);

    uris = soup_server_get_uris(fake->server);
    fake->url = g_uri_to_string(uris->data);
    g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);

    return fake;
}

static void
fake_server_stop(FakeServer *fake)
{
    soup_server_disconnect(fake->server);
    g_clear_object(&fake->server);
    g_free(fake->url);
    g_free(fake->last_path);
    g_free(fake->last_query);
    g_free(fake);
}

/* ── Tools ────────────────────────────────────────────────────────── */

typedef struct {
    gchar             *dir;
    ClawtConfig       *config;
    ClawtAgentManager *agents;
    ClawtTaskManager  *tasks;
    ClawtLoopGuard    *guard;
    ClawtMcpTools     *tools;
    GMainContext      *context;
    gchar             *search_url;   /* NULL: the hook answers nobody */
} Fixture;

static AiLibresearchSearch *
fake_search(const gchar *agent_id, gpointer user_data, GError **error)
{
    Fixture *fixture = user_data;

    (void)error;
    if (fixture->search_url == NULL || g_strcmp0(agent_id, "researcher") != 0)
        return NULL;
    return ai_libresearch_search_new(fixture->search_url, NULL);
}

static void
fixture_setup(Fixture *fixture)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *yaml = NULL;

    fixture->dir = g_dir_make_tmp("clawt-web-XXXXXX", NULL);
    yaml = g_strdup_printf("daemon:\n  state_dir: \"%s\"\n"
                           "defaults:\n  workspace_root: \"%s/agents\"\n"
                           "agents:\n  - id: researcher\n  - id: other\n",
                           fixture->dir, fixture->dir);
    fixture->config = clawt_config_load_from_string(yaml, &error);
    g_assert_no_error(error);

    fixture->agents = clawt_agent_manager_new(fixture->config);
    clawt_agent_manager_load(fixture->agents, NULL);
    fixture->tasks = clawt_task_manager_new();
    fixture->guard = clawt_loop_guard_new();
    fixture->tools = clawt_mcp_tools_new(fixture->agents, fixture->tasks, fixture->guard);

    /* A context of its own, named to the tools: the answer must land here. */
    fixture->context = g_main_context_new();
    clawt_mcp_tools_set_main_context(fixture->tools, fixture->context);
    clawt_mcp_tools_set_search_func(fixture->tools, fake_search, fixture, NULL);
}

static void
fixture_teardown(Fixture *fixture)
{
    g_clear_object(&fixture->tools);
    g_clear_object(&fixture->guard);
    g_clear_object(&fixture->tasks);
    g_clear_object(&fixture->agents);
    g_clear_object(&fixture->config);
    g_clear_pointer(&fixture->context, g_main_context_unref);
    g_clear_pointer(&fixture->search_url, g_free);
    clawt_test_remove_tree(fixture->dir);
    g_clear_pointer(&fixture->dir, g_free);
}

static gboolean
listing_has(JsonNode *listing, const gchar *name)
{
    g_autofree gchar *text = json_to_string(listing, FALSE);
    g_autofree gchar *needle = g_strdup_printf("\"%s\"", name);

    return strstr(text, needle) != NULL;
}

/* Offered exactly when the hook answers for the agent. */
static void
test_tools_offered_with_a_server(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) without = NULL;
    g_autoptr(JsonNode) with = NULL;
    g_autoptr(JsonNode) other = NULL;

    fixture_setup(&fixture);

    without = clawt_mcp_tools_list(fixture.tools, "researcher");
    g_assert_false(listing_has(without, "clawtilla_web_query"));

    fixture.search_url = g_strdup("http://127.0.0.1:9");
    with = clawt_mcp_tools_list(fixture.tools, "researcher");
    other = clawt_mcp_tools_list(fixture.tools, "other");
    g_assert_true(listing_has(with, "clawtilla_web_search"));
    g_assert_true(listing_has(with, "clawtilla_web_query"));
    g_assert_true(listing_has(with, "clawtilla_web_research"));
    g_assert_true(listing_has(with, "clawtilla_web_query_environment"));
    g_assert_false(listing_has(other, "clawtilla_web_search"));

    fixture_teardown(&fixture);
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

static const gchar *
result_text(JsonNode *response, gboolean *is_error)
{
    JsonObject *result = json_object_get_object_member(json_node_get_object(response), "result");
    JsonArray *content = json_object_get_array_member(result, "content");

    *is_error = json_object_has_member(result, "isError") &&
                json_object_get_boolean_member(result, "isError");
    return json_object_get_string_member(json_array_get_object_element(content, 0), "text");
}

/* Runs one web tool through the deferred path and waits, with a watchdog. */
static JsonNode *
run_web_tool(Fixture *fixture, const gchar *tool, const gchar *arguments)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autofree gchar *request = g_strdup_printf(
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"%s\",\"arguments\":%s}}", tool, arguments);
    Probe probe = { FALSE, NULL };
    gint64 deadline;

    g_assert_true(json_parser_load_from_data(parser, request, -1, NULL));
    g_assert_true(clawt_mcp_tools_call_defers(fixture->tools, "researcher",
                                              json_parser_get_root(parser)));

    clawt_mcp_tools_call_async(fixture->tools, "researcher", NULL,
                               json_parser_get_root(parser), on_done, &probe);

    /*
     * Iterating the tools' context *and* the default one: the soup server
     * answers on the default, the client on the context the tools named.
     * The tools' context is pushed for the whole wait, as it is while a
     * daemon runs: libsoup creates sources on the thread-default from
     * inside its own callbacks, so a session's context iterated without
     * being thread-default sends a request whose answer is never read --
     * measured, not assumed, in a ten-line program.
     */
    g_main_context_push_thread_default(fixture->context);
    deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
    while (!probe.arrived && g_get_monotonic_time() < deadline) {
        g_main_context_iteration(fixture->context, FALSE);
        g_main_context_iteration(g_main_context_default(), FALSE);
    }
    g_main_context_pop_thread_default(fixture->context);

    g_assert_true(probe.arrived);
    return probe.response;
}

static void
test_query_runs_deferred(void)
{
    Fixture fixture = { 0 };
    FakeServer *fake = fake_server_start();
    g_autoptr(JsonNode) response = NULL;
    gboolean is_error = TRUE;
    const gchar *text;

    fixture_setup(&fixture);
    fixture.search_url = g_strdup(fake->url);

    response = run_web_tool(&fixture, "clawtilla_web_query",
                            "{\"query\":\"search \\\"gtk\\\" | fields url, title\",\"explain\":false}");
    text = result_text(response, &is_error);

    g_assert_false(is_error);
    g_assert_cmpstr(fake->last_path, ==, "/api/v1/query");
    g_assert_nonnull(strstr(fake->last_query, "format=json"));
    g_assert_nonnull(strstr(text, "1. url: https://docs.gtk.org/"));
    g_assert_nonnull(strstr(text, "title: GTK Docs"));

    fixture_teardown(&fixture);
    fake_server_stop(fake);
}

/* The server's own words come back, as an error the agent can read. */
static void
test_research_error_is_the_servers(void)
{
    Fixture fixture = { 0 };
    FakeServer *fake = fake_server_start();
    g_autoptr(JsonNode) response = NULL;
    gboolean is_error = FALSE;
    const gchar *text;

    fixture_setup(&fixture);
    fixture.search_url = g_strdup(fake->url);

    response = run_web_tool(&fixture, "clawtilla_web_research",
                            "{\"question\":\"what is LQL\",\"deep\":true}");
    text = result_text(response, &is_error);

    g_assert_true(is_error);
    g_assert_nonnull(strstr(text, "research module is not active"));
    g_assert_nonnull(strstr(fake->last_query, "deep=1"));

    fixture_teardown(&fixture);
    fake_server_stop(fake);
}

/* A missing required argument is answered at once, through the callback. */
static void
test_missing_argument_refused(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) response = NULL;
    gboolean is_error = FALSE;

    fixture_setup(&fixture);
    fixture.search_url = g_strdup("http://127.0.0.1:9");

    response = run_web_tool(&fixture, "clawtilla_web_query", "{}");
    g_assert_cmpstr(result_text(response, &is_error), ==, "query is required");
    g_assert_true(is_error);

    fixture_teardown(&fixture);
}

/* ── The bundled skill ────────────────────────────────────────────── */

static void
test_bundled_skill_installs_and_scans(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-skills-XXXXXX", NULL);
    g_autofree gchar *skill = g_build_filename(dir, "web-research", "SKILL.md", NULL);
    g_autofree gchar *reference = g_build_filename(dir, "web-research", "references", "lql.md", NULL);
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtSkillLibrary) library = NULL;
    ClawtSkill *found;

    g_assert_true(clawt_skill_install_bundled(dir, &error));
    g_assert_no_error(error);
    g_assert_true(g_file_test(skill, G_FILE_TEST_EXISTS));
    g_assert_true(g_file_test(reference, G_FILE_TEST_EXISTS));

    library = clawt_skill_library_new(dir);
    clawt_skill_library_scan(library);
    found = clawt_skill_library_lookup(library, "web-research");
    g_assert_nonnull(found);
    g_assert_true(clawt_skill_get_enabled(found));
    g_assert_nonnull(strstr(clawt_skill_get_description(found), "LQL"));
    g_assert_cmpuint(clawt_skill_library_get_problems(library)->len, ==, 0);

    clawt_test_remove_tree(dir);
}

/* Somebody's own skill by that name is theirs; a stale copy of ours is replaced. */
static void
test_bundled_skill_ownership(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-skills-XXXXXX", NULL);
    g_autofree gchar *skill_dir = g_build_filename(dir, "web-research", NULL);
    g_autofree gchar *skill = g_build_filename(skill_dir, "SKILL.md", NULL);
    g_autofree gchar *marker = g_build_filename(skill_dir, CLAWT_SKILL_BUNDLED_MARKER, NULL);
    g_autofree gchar *contents = NULL;
    g_autoptr(GError) error = NULL;

    g_assert_cmpint(g_mkdir_with_parents(skill_dir, 0700), ==, 0);
    g_assert_true(g_file_set_contents(skill, "mine", -1, NULL));
    g_assert_true(clawt_skill_install_bundled(dir, &error));
    g_assert_true(g_file_get_contents(skill, &contents, NULL, NULL));
    g_assert_cmpstr(contents, ==, "mine");
    g_clear_pointer(&contents, g_free);

    /* Now ours, but from an older clawtilla: the digest differs. */
    g_assert_true(g_file_set_contents(marker, "stale-digest", -1, NULL));
    g_assert_true(clawt_skill_install_bundled(dir, &error));
    g_assert_no_error(error);
    g_assert_true(g_file_get_contents(skill, &contents, NULL, NULL));
    g_assert_nonnull(strstr(contents, "name: web-research"));

    clawt_test_remove_tree(dir);
}

/* Assigned with the integration, and only with it. */
static void
test_skill_assigned_with_integration(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtConfig) config = clawt_config_load_from_string(
        "agents:\n"
        "  - id: researcher\n"
        "    integrations:\n"
        "      libresearch:\n"
        "        enabled: true\n"
        "        url: http://127.0.0.1:8087\n"
        "  - id: plain\n", &error);
    g_autoptr(GPtrArray) with = NULL;
    g_autoptr(GPtrArray) without = NULL;
    guint i;
    gboolean found = FALSE;

    g_assert_no_error(error);
    with = clawt_skill_resolve_for_agent(config, clawt_config_get_agent(config, "researcher"), NULL);
    without = clawt_skill_resolve_for_agent(config, clawt_config_get_agent(config, "plain"), NULL);

    for (i = 0; i < with->len; i++) {
        ClawtSkillBinding *binding = g_ptr_array_index(with, i);

        if (g_strcmp0(clawt_skill_binding_get_name(binding), "web-research") == 0) {
            found = TRUE;
            g_assert_cmpstr(clawt_skill_binding_get_origin(binding), ==, "integration");
        }
    }
    g_assert_true(found);
    g_assert_cmpuint(without->len, ==, 0);
}

/*
 * What the agent reads in TOOLS.org: a pointer to the skill where its CLI
 * loads skills, the reference itself where it cannot (codex).
 */
static gchar *
tools_org_for(const gchar *provider)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *dir = g_dir_make_tmp("clawt-web-ws-XXXXXX", NULL);
    g_autofree gchar *yaml = g_strdup_printf(
        "defaults:\n  workspace_root: \"%s\"\n"
        "agents:\n"
        "  - id: researcher\n"
        "    model:\n      provider: %s\n"
        "    integrations:\n"
        "      libresearch:\n        enabled: true\n        url: http://127.0.0.1:8087\n",
        dir, provider);
    g_autoptr(ClawtConfig) config = clawt_config_load_from_string(yaml, &error);
    ClawtAgentConfig *agent;
    g_autofree gchar *path = NULL;
    g_autofree gchar *workspace = NULL;
    gchar *text = NULL;

    g_assert_no_error(error);
    agent = clawt_config_get_agent(config, "researcher");
    g_assert_true(clawt_workspace_scaffold(agent, &error));
    g_assert_true(clawt_workspace_update_tools_org(config, agent, &error));
    g_assert_no_error(error);

    workspace = clawt_agent_config_get_workspace(agent);
    path = g_build_filename(workspace, "TOOLS.org", NULL);
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
    clawt_test_remove_tree(dir);
    return text;
}

static void
test_tools_org_points_or_inlines(void)
{
    g_autofree gchar *claude = tools_org_for("claude-code");
    g_autofree gchar *codex = tools_org_for("codex-cli");

    g_assert_nonnull(strstr(claude, "clawtilla_web_query"));
    g_assert_nonnull(strstr(claude, "The ~web-research~ skill"));
    g_assert_null(strstr(claude, "The LQL reference, since your CLI"));

    g_assert_nonnull(strstr(codex, "The LQL reference, since your CLI does not load skills"));
    g_assert_nonnull(strstr(codex, "## The `search` source"));
}

gint
main(gint argc, gchar *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/web-research/tools-offered-with-a-server", test_tools_offered_with_a_server);
    g_test_add_func("/web-research/query-runs-deferred", test_query_runs_deferred);
    g_test_add_func("/web-research/research-error-is-the-servers", test_research_error_is_the_servers);
    g_test_add_func("/web-research/missing-argument-refused", test_missing_argument_refused);
    g_test_add_func("/web-research/bundled-skill-installs", test_bundled_skill_installs_and_scans);
    g_test_add_func("/web-research/bundled-skill-ownership", test_bundled_skill_ownership);
    g_test_add_func("/web-research/skill-assigned-with-integration", test_skill_assigned_with_integration);
    g_test_add_func("/web-research/tools-org-points-or-inlines", test_tools_org_points_or_inlines);

    return g_test_run();
}
