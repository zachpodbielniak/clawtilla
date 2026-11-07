/*
 * test-policy.c - Action rules
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The rules themselves (parsing, first match, the fleet fallback, the
 * classes of the CLI's own tools); the gate on clawtilla's tools (deny
 * refuses at once, a waiting rule defers and runs only on a yes); the
 * daemon's ask loop end to end (a decision is posted, answering approve
 * releases the call, a deadline settles it DEFAULTED and refuses); and the
 * Claude hook's settings, read back by libreclaw's own parser.
 *
 * The daemon half includes core/clawt-daemon-private.h, as
 * test-turn-hygiene does, to drive the ask loop without a socket.
 */

#include "clawtilla.h"
#include "clawt-test-util.h"
#include "core/clawt-daemon-private.h"

#include <glib/gstdio.h>
#include <libreclaw.h>
#include <string.h>

/* ── The rules ────────────────────────────────────────────────────── */

static void
test_parse_rule(void)
{
    g_autofree gchar *match = NULL;
    g_autoptr(GError) error = NULL;
    ClawtPolicyMode mode = CLAWT_POLICY_ALLOW;

    g_assert_true(clawt_policy_parse_rule(" exec = ask ", &match, &mode, &error));
    g_assert_no_error(error);
    g_assert_cmpstr(match, ==, "exec");
    g_assert_cmpint(mode, ==, CLAWT_POLICY_ASK);

    g_assert_false(clawt_policy_parse_rule("exec=maybe", NULL, NULL, &error));
    g_assert_nonnull(strstr(error->message, "hand_back"));   /* names the modes */
    g_clear_error(&error);

    g_assert_false(clawt_policy_parse_rule("exec", NULL, NULL, &error));
    g_clear_error(&error);
    g_assert_false(clawt_policy_parse_rule("=deny", NULL, NULL, &error));
}

static ClawtConfig *
load(const gchar *yaml)
{
    g_autoptr(GError) error = NULL;
    ClawtConfig *config = clawt_config_load_from_string(yaml, &error);

    g_assert_no_error(error);
    return config;
}

static void
test_first_match_and_fallback(void)
{
    g_autoptr(ClawtConfig) config = load(
        "defaults:\n  policy:\n    rules: [\"delete=deny\"]\n"
        "agents:\n"
        "  - id: own\n"
        "    policy:\n      rules: [\"clawtilla_create_*=hand_back\", \"exec=ask\", \"exec=deny\"]\n"
        "  - id: fleet\n");
    ClawtAgentConfig *own = clawt_config_get_agent(config, "own");
    ClawtAgentConfig *fleet = clawt_config_get_agent(config, "fleet");
    g_autofree gchar *rule = NULL;

    /* First match wins: exec=ask, not exec=deny. */
    g_assert_cmpint(clawt_policy_mode_for(own, "Bash", CLAWT_ACTION_EXEC, &rule), ==,
                    CLAWT_POLICY_ASK);
    g_assert_cmpstr(rule, ==, "exec=ask");
    g_clear_pointer(&rule, g_free);

    /* A glob on the tool's name. */
    g_assert_cmpint(clawt_policy_mode_for(own, "clawtilla_create_agent",
                                          CLAWT_ACTION_FLEET_ADMIN, NULL), ==,
                    CLAWT_POLICY_HAND_BACK);

    /* The agent's list replaces the fleet's: no delete rule for `own`. */
    g_assert_cmpint(clawt_policy_mode_for(own, "x", CLAWT_ACTION_DELETE, NULL), ==,
                    CLAWT_POLICY_ALLOW);
    g_assert_cmpint(clawt_policy_mode_for(fleet, "x", CLAWT_ACTION_DELETE, NULL), ==,
                    CLAWT_POLICY_DENY);

    /* Nothing matches: allowed. */
    g_assert_cmpint(clawt_policy_mode_for(fleet, "Read", CLAWT_ACTION_READ, &rule), ==,
                    CLAWT_POLICY_ALLOW);
    g_assert_null(rule);
}

static void
test_bad_rule_shadows(void)
{
    g_autoptr(ClawtConfig) config = load(
        "agents:\n  - id: typo\n    policy:\n      rules: [\"exec=aks\"]\n");
    ClawtAgentConfig *agent = clawt_config_get_agent(config, "typo");

    g_assert_true(clawt_agent_config_is_shadow(agent));
    g_assert_nonnull(strstr(clawt_agent_config_get_shadow_reason(agent), "policy.rules"));
}

/* An unknown tool is never read: the guess falls on the side that asks. */
static void
test_native_classes(void)
{
    GEnumClass *klass = g_type_class_ref(CLAWT_TYPE_ACTION_CLASS);

    g_assert_cmpint(clawt_policy_class_for_native_tool("Bash"), ==, CLAWT_ACTION_EXEC);
    g_assert_cmpint(clawt_policy_class_for_native_tool("Write"), ==, CLAWT_ACTION_WRITE);
    g_assert_cmpint(clawt_policy_class_for_native_tool("Read"), ==, CLAWT_ACTION_READ);
    g_assert_cmpint(clawt_policy_class_for_native_tool("mcp__github__create_issue"), ==,
                    CLAWT_ACTION_EXTERNAL);
    g_assert_cmpint(clawt_policy_class_for_native_tool("SomethingNew"), ==, CLAWT_ACTION_EXEC);
    g_assert_true(clawt_policy_native_is_clawtilla("mcp__clawtilla__clawtilla_list_agents"));
    g_assert_false(clawt_policy_native_is_clawtilla("mcp__github__x"));

    /* Every class nickname parses as a rule's match. */
    {
        guint i;

        for (i = 0; i < klass->n_values; i++) {
            g_autofree gchar *rule = g_strdup_printf("%s=deny", klass->values[i].value_nick);

            g_assert_true(clawt_policy_parse_rule(rule, NULL, NULL, NULL));
        }
    }
    g_type_class_unref(klass);
}

/* ── The gate on clawtilla's tools ────────────────────────────────── */

typedef struct {
    gchar             *dir;
    ClawtConfig       *config;
    ClawtAgentManager *agents;
    ClawtTaskManager  *tasks;
    ClawtLoopGuard    *guard;
    ClawtMcpTools     *tools;
    gboolean           approve;
    guint              asked;
    gchar             *asked_rule;
} ToolsFixture;

static void
fake_policy(const gchar *agent_id, const gchar *tool, ClawtActionClass action,
            ClawtPolicyMode mode, const gchar *rule, const gchar *summary,
            ClawtPolicyReplyFunc reply, gpointer reply_data, gpointer user_data)
{
    ToolsFixture *fixture = user_data;

    (void)agent_id; (void)tool; (void)action; (void)mode; (void)summary;
    fixture->asked++;
    g_free(fixture->asked_rule);
    fixture->asked_rule = g_strdup(rule);
    reply(fixture->approve, fixture->approve ? NULL : "the operator said no", reply_data);
}

static void
tools_setup(ToolsFixture *fixture, const gchar *rules, gboolean with_hook)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *yaml = NULL;

    fixture->dir = g_dir_make_tmp("clawt-policy-XXXXXX", NULL);
    yaml = g_strdup_printf("daemon:\n  state_dir: \"%s\"\n"
                           "defaults:\n  workspace_root: \"%s/agents\"\n"
                           "agents:\n  - id: chief\n    policy:\n      rules: [%s]\n",
                           fixture->dir, fixture->dir, rules);
    fixture->config = clawt_config_load_from_string(yaml, &error);
    g_assert_no_error(error);
    fixture->agents = clawt_agent_manager_new(fixture->config);
    clawt_agent_manager_load(fixture->agents, NULL);
    fixture->tasks = clawt_task_manager_new();
    fixture->guard = clawt_loop_guard_new();
    fixture->tools = clawt_mcp_tools_new(fixture->agents, fixture->tasks, fixture->guard);
    if (with_hook)
        clawt_mcp_tools_set_policy_func(fixture->tools, fake_policy, fixture, NULL);
}

static void
tools_teardown(ToolsFixture *fixture)
{
    g_clear_object(&fixture->tools);
    g_clear_object(&fixture->guard);
    g_clear_object(&fixture->tasks);
    g_clear_object(&fixture->agents);
    g_clear_object(&fixture->config);
    g_clear_pointer(&fixture->asked_rule, g_free);
    clawt_test_remove_tree(fixture->dir);
    g_clear_pointer(&fixture->dir, g_free);
}

static JsonNode *
request_for(const gchar *tool)
{
    g_autofree gchar *text = g_strdup_printf(
        "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"%s\",\"arguments\":{}}}", tool);

    return json_from_string(text, NULL);
}

static const gchar *
text_of(JsonNode *response, gboolean *is_error)
{
    JsonObject *result = json_object_get_object_member(json_node_get_object(response), "result");

    *is_error = json_object_get_boolean_member_with_default(result, "isError", FALSE);
    return json_object_get_string_member(
        json_array_get_object_element(json_object_get_array_member(result, "content"), 0),
        "text");
}

static void
test_deny_refuses_at_once(void)
{
    ToolsFixture fixture = { 0 };
    g_autoptr(JsonNode) request = NULL;
    g_autoptr(JsonNode) response = NULL;
    gboolean is_error = FALSE;

    tools_setup(&fixture, "\"read=deny\"", TRUE);
    request = request_for("clawtilla_list_agents");

    g_assert_false(clawt_mcp_tools_call_defers(fixture.tools, "chief", request));
    response = clawt_mcp_tools_call(fixture.tools, "chief", NULL, request);
    g_assert_nonnull(strstr(text_of(response, &is_error), "'read=deny'"));
    g_assert_true(is_error);
    g_assert_cmpuint(fixture.asked, ==, 0);

    tools_teardown(&fixture);
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
call_deferred(ToolsFixture *fixture, JsonNode *request)
{
    Probe probe = { FALSE, NULL };
    gint64 deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;

    clawt_mcp_tools_call_async(fixture->tools, "chief", NULL, request, on_done, &probe);
    while (!probe.arrived && g_get_monotonic_time() < deadline)
        g_main_context_iteration(NULL, FALSE);
    g_assert_true(probe.arrived);
    return probe.response;
}

/* A waiting rule defers even a tool that normally answers at once. */
static void
test_ask_defers_and_runs_on_yes(void)
{
    ToolsFixture fixture = { 0 };
    g_autoptr(JsonNode) request = NULL;
    g_autoptr(JsonNode) yes = NULL;
    g_autoptr(JsonNode) no = NULL;
    gboolean is_error = TRUE;

    tools_setup(&fixture, "\"read=ask\"", TRUE);
    request = request_for("clawtilla_list_agents");

    g_assert_true(clawt_mcp_tools_call_defers(fixture.tools, "chief", request));

    fixture.approve = TRUE;
    yes = call_deferred(&fixture, request);
    /* The tool's own answer, not a refusal: it ran. */
    g_assert_nonnull(strstr(text_of(yes, &is_error), "only agent in this fleet"));
    g_assert_false(is_error);
    g_assert_cmpstr(fixture.asked_rule, ==, "read=ask");

    fixture.approve = FALSE;
    no = call_deferred(&fixture, request);
    g_assert_cmpstr(text_of(no, &is_error), ==, "the operator said no");
    g_assert_true(is_error);
    g_assert_cmpuint(fixture.asked, ==, 2);

    /* And the route that cannot wait refuses rather than running unasked. */
    {
        g_autoptr(JsonNode) sync = clawt_mcp_tools_call(fixture.tools, "chief", NULL, request);

        g_assert_nonnull(strstr(text_of(sync, &is_error), "cannot wait"));
        g_assert_true(is_error);
    }

    tools_teardown(&fixture);
}

/* With nobody to ask, a rule that asks is a refusal, never a pass. */
static void
test_ask_without_inbox_refuses(void)
{
    ToolsFixture fixture = { 0 };
    g_autoptr(JsonNode) request = NULL;
    g_autoptr(JsonNode) response = NULL;
    gboolean is_error = FALSE;

    tools_setup(&fixture, "\"read=ask\"", FALSE);
    request = request_for("clawtilla_list_agents");
    response = call_deferred(&fixture, request);
    g_assert_nonnull(strstr(text_of(response, &is_error), "nobody here to ask"));
    g_assert_true(is_error);

    tools_teardown(&fixture);
}

/* ── The daemon's ask loop ────────────────────────────────────────── */

typedef struct {
    gchar        *dir;
    GMainContext *context;
    ClawtDaemon  *daemon;
} DaemonFixture;

static void
daemon_setup(DaemonFixture *fixture)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *path = NULL;
    g_autofree gchar *yaml = NULL;

    fixture->dir = g_dir_make_tmp("clawt-policy-d-XXXXXX", NULL);
    path = g_build_filename(fixture->dir, "config.yaml", NULL);
    yaml = g_strdup_printf("daemon:\n  tailscale: false\n  state_dir: \"%s/state\"\n"
                           "  socket: \"%s/d.sock\"\n  automation_dir: \"%s/pods\"\n"
                           "defaults:\n  workspace_root: \"%s/agents\"\n"
                           "  policy:\n    ask_timeout_seconds: 1\n"
                           "agents:\n  - id: worker\n    runtime:\n      autostart: false\n",
                           fixture->dir, fixture->dir, fixture->dir, fixture->dir);
    g_assert_true(g_file_set_contents(path, yaml, -1, &error));
    fixture->context = g_main_context_new();
    fixture->daemon = clawt_daemon_new(path, fixture->context);
    g_assert_true(clawt_daemon_start(fixture->daemon, NULL));
}

static void
daemon_teardown(DaemonFixture *fixture)
{
    clawt_daemon_stop(fixture->daemon);
    g_clear_object(&fixture->daemon);
    while (g_main_context_iteration(fixture->context, FALSE))
        ;
    g_clear_pointer(&fixture->context, g_main_context_unref);
    clawt_test_remove_tree(fixture->dir);
    g_free(fixture->dir);
}

typedef struct {
    gboolean  replied;
    gboolean  allowed;
    gchar    *refusal;
} Reply;

static void
on_reply(gboolean allowed, const gchar *refusal, gpointer data)
{
    Reply *reply = data;

    reply->replied = TRUE;
    reply->allowed = allowed;
    reply->refusal = g_strdup(refusal);
}

/* The only open decision's id, which the ask loop generated. */
static gchar *
open_decision_id(ClawtDaemon *daemon)
{
    g_autoptr(GPtrArray) open = clawt_decision_store_list(daemon->decisions, TRUE);

    g_assert_cmpuint(open->len, ==, 1);
    return g_strdup(clawt_decision_get_id(g_ptr_array_index(open, 0)));
}

static void
test_daemon_ask_answered(void)
{
    DaemonFixture fixture = { 0 };
    Reply reply = { FALSE, FALSE, NULL };
    g_autofree gchar *id = NULL;
    g_autofree gchar *payload = NULL;
    g_autoptr(JsonNode) frame = NULL;
    g_autoptr(JsonNode) answered = NULL;
    g_autoptr(JsonParser) parser = json_parser_new();

    daemon_setup(&fixture);
    clawt_daemon_policy_request(fixture.daemon, "worker", "Bash", CLAWT_ACTION_EXEC,
                                CLAWT_POLICY_ASK, "exec=ask", "Bash {\"command\":\"ls\"}",
                                on_reply, &reply);
    g_assert_false(reply.replied);

    id = open_decision_id(fixture.daemon);
    g_assert_true(g_str_has_prefix(id, "policy-"));

    payload = g_strdup_printf("{\"decision\":\"%s\",\"answer\":\"approve\"}", id);
    g_assert_true(json_parser_load_from_data(parser, payload, -1, NULL));
    frame = clawt_ipc_request_new("decision.answer", "t1");
    clawt_ipc_frame_set_payload(frame, json_node_copy(json_parser_get_root(parser)));
    answered = clawt_daemon_handle_request(fixture.daemon, frame);

    g_assert_true(reply.replied);
    g_assert_true(reply.allowed);
    g_assert_null(reply.refusal);

    daemon_teardown(&fixture);
}

/* Nobody answers: DEFAULTED, not answered, and the call is refused. */
static void
test_daemon_ask_defaults(void)
{
    DaemonFixture fixture = { 0 };
    Reply reply = { FALSE, FALSE, NULL };
    g_autofree gchar *id = NULL;
    g_autoptr(ClawtDecision) settled = NULL;

    daemon_setup(&fixture);
    clawt_daemon_policy_request(fixture.daemon, "worker", "Bash", CLAWT_ACTION_EXEC,
                                CLAWT_POLICY_ASK, "exec=ask", "Bash", on_reply, &reply);
    id = open_decision_id(fixture.daemon);

    g_usleep(2100 * 1000);            /* past the one-second deadline */
    clawt_daemon_policy_tick(fixture.daemon);

    g_assert_true(reply.replied);
    g_assert_false(reply.allowed);
    g_assert_nonnull(strstr(reply.refusal, "Nobody answered"));

    settled = clawt_decision_store_get(fixture.daemon->decisions, id);
    g_assert_cmpint(clawt_decision_get_state(settled), ==, CLAWT_DECISION_DEFAULTED);
    g_assert_cmpstr(clawt_decision_get_answer(settled), ==, "deny");

    g_free(reply.refusal);
    daemon_teardown(&fixture);
}

/* A decision an agent asked for itself defaults too, and is said to have. */
static void
test_agent_decision_defaults(void)
{
    DaemonFixture fixture = { 0 };
    g_autoptr(ClawtDecision) question = clawt_decision_new(NULL, "worker", "Which?");
    g_autofree gchar *id = NULL;
    g_autoptr(ClawtDecision) settled = NULL;
    static const gchar * const options[] = { "a", "b", NULL };

    daemon_setup(&fixture);
    clawt_decision_set_options(question, options);
    clawt_decision_set_default(question, "a", "the safer one");
    clawt_decision_set_reversible_until(question, g_get_real_time() / G_USEC_PER_SEC - 1);
    id = clawt_decision_store_post(fixture.daemon->decisions, question, NULL);

    clawt_daemon_policy_tick(fixture.daemon);
    settled = clawt_decision_store_get(fixture.daemon->decisions, id);
    g_assert_cmpint(clawt_decision_get_state(settled), ==, CLAWT_DECISION_DEFAULTED);
    g_assert_cmpstr(clawt_decision_get_answer(settled), ==, "a");

    daemon_teardown(&fixture);
}

static void
test_hand_back_refuses_and_files(void)
{
    DaemonFixture fixture = { 0 };
    Reply reply = { FALSE, FALSE, NULL };
    g_autoptr(GPtrArray) open = NULL;

    daemon_setup(&fixture);
    clawt_daemon_policy_request(fixture.daemon, "worker", "clawtilla_create_agent",
                                CLAWT_ACTION_FLEET_ADMIN, CLAWT_POLICY_HAND_BACK,
                                "fleet_admin=hand_back", "clawtilla_create_agent {}",
                                on_reply, &reply);
    g_assert_true(reply.replied);
    g_assert_false(reply.allowed);
    g_assert_nonnull(strstr(reply.refusal, "Handed to the operator"));

    open = clawt_decision_store_list(fixture.daemon->decisions, TRUE);
    g_assert_cmpuint(open->len, ==, 1);

    g_free(reply.refusal);
    daemon_teardown(&fixture);
}

/* ── The hook's settings ──────────────────────────────────────────── */

static void
test_hook_settings_follow_rules(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *dir = g_dir_make_tmp("clawt-policy-h-XXXXXX", NULL);
    g_autofree gchar *yaml = g_strdup_printf(
        "defaults:\n  workspace_root: \"%s/agents\"\n"
        "agents:\n  - id: ruled\n    policy:\n      rules: [\"exec=ask\"]\n  - id: free\n",
        dir);
    g_autoptr(ClawtConfig) config = load(yaml);
    g_autofree gchar *state = g_build_filename(dir, "state", NULL);
    g_autofree gchar *settings = clawt_workspace_claude_settings_path(state);
    g_autofree gchar *contents = NULL;
    g_autofree gchar *rendered = NULL;
    g_autofree gchar *rendered_path = g_build_filename(dir, "rendered.yaml", NULL);
    g_autoptr(LcConfig) parsed = lc_config_new();
    ClawtAgentConfig *ruled = clawt_config_get_agent(config, "ruled");
    ClawtAgentConfig *free_agent = clawt_config_get_agent(config, "free");

    g_assert_true(clawt_workspace_scaffold(ruled, &error));
    clawt_workspace_write_mcp_config(config, ruled, "/run/d.sock", state, &error);
    g_assert_no_error(error);
    g_assert_true(g_file_get_contents(settings, &contents, NULL, NULL));
    g_assert_nonnull(strstr(contents, "\"PreToolUse\""));
    g_assert_nonnull(strstr(contents, "policy-hook --agent 'ruled'"));

    rendered = clawt_config_render_agent(config, ruled, "/tmp/s.sock", state, NULL);
    g_assert_true(g_file_set_contents(rendered_path, rendered, -1, NULL));
    g_assert_true(lc_config_load_from_path(parsed, rendered_path, &error));
    g_assert_cmpstr(lc_config_get_ai_claude_settings(parsed), ==, settings);

    /* No rules: no file, no setting. The free agent pays nothing per call. */
    g_assert_true(clawt_workspace_scaffold(free_agent, &error));
    g_assert_true(clawt_workspace_write_mcp_config(config, free_agent, "/run/d.sock", state,
                                                   &error));
    g_assert_false(g_file_test(settings, G_FILE_TEST_EXISTS));
    g_clear_pointer(&rendered, g_free);
    rendered = clawt_config_render_agent(config, free_agent, "/tmp/s.sock", state, NULL);
    g_assert_null(strstr(rendered, "settings_path"));

    clawt_test_remove_tree(dir);
}

gint
main(gint argc, gchar *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/policy/parse-rule", test_parse_rule);
    g_test_add_func("/policy/first-match-and-fallback", test_first_match_and_fallback);
    g_test_add_func("/policy/bad-rule-shadows", test_bad_rule_shadows);
    g_test_add_func("/policy/native-classes", test_native_classes);
    g_test_add_func("/policy/tools/deny-refuses-at-once", test_deny_refuses_at_once);
    g_test_add_func("/policy/tools/ask-defers-and-runs-on-yes", test_ask_defers_and_runs_on_yes);
    g_test_add_func("/policy/tools/ask-without-inbox-refuses", test_ask_without_inbox_refuses);
    g_test_add_func("/policy/daemon/ask-answered", test_daemon_ask_answered);
    g_test_add_func("/policy/daemon/ask-defaults", test_daemon_ask_defaults);
    g_test_add_func("/policy/daemon/agent-decision-defaults", test_agent_decision_defaults);
    g_test_add_func("/policy/daemon/hand-back", test_hand_back_refuses_and_files);
    g_test_add_func("/policy/hook-settings-follow-rules", test_hook_settings_follow_rules);

    return g_test_run();
}
