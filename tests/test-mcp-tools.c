/*
 * test-mcp-tools.c - The tools agents use to work together
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * These tools are what a chief-of-staff actually calls to delegate, so the
 * tests are mostly about permissions and about what an agent is told when
 * something is refused -- a model that gets an unexplained failure tries
 * three variations of the same call.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>

#include "clawt-test-util.h"

typedef struct {
    gchar             *dir;
    ClawtConfig       *config;
    ClawtAgentManager *agents;
    ClawtTaskManager  *tasks;
    ClawtLoopGuard    *guard;
    ClawtRoomManager  *rooms;
    ClawtMcpTools     *tools;

    gchar             *last_target;
    gchar             *last_body;
    gboolean           deliver_fails;
} Fixture;

static gboolean
fake_deliver(const gchar  *from_agent,
             const gchar  *target,
             const gchar  *body,
             const gchar  *task_id,
             gint          depth,
             gpointer      user_data,
             GError      **error)
{
    Fixture *fixture = user_data;

    if (fixture->deliver_fails) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT,
                            "refused by the loop guard");
        return FALSE;
    }

    g_free(fixture->last_target);
    g_free(fixture->last_body);
    fixture->last_target = g_strdup(target);
    fixture->last_body = g_strdup(body);

    return TRUE;
}

static void
fixture_setup(Fixture *fixture, const gchar *agents_yaml)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *yaml = NULL;

    fixture->dir = g_dir_make_tmp("clawt-mcp-XXXXXX", NULL);

    yaml = g_strdup_printf("daemon:\n  state_dir: \"%s\"\n%s",
                           fixture->dir, agents_yaml);

    fixture->config = clawt_config_load_from_string(yaml, &error);
    g_assert_no_error(error);

    fixture->agents = clawt_agent_manager_new(fixture->config);
    clawt_agent_manager_load(fixture->agents, NULL);

    fixture->tasks = clawt_task_manager_new();
    fixture->guard = clawt_loop_guard_new();
    fixture->rooms = clawt_room_manager_new(NULL);
    fixture->tools = clawt_mcp_tools_new(fixture->agents, fixture->tasks,
                                         fixture->guard);
    clawt_mcp_tools_set_room_manager(fixture->tools, fixture->rooms);

    clawt_mcp_tools_set_deliver_func(fixture->tools, fake_deliver, fixture,
                                     NULL);
}

static void
fixture_teardown(Fixture *fixture)
{
    g_clear_object(&fixture->tools);
    g_clear_object(&fixture->rooms);
    g_clear_object(&fixture->guard);
    g_clear_object(&fixture->tasks);
    g_clear_object(&fixture->agents);
    g_clear_object(&fixture->config);

    g_clear_pointer(&fixture->last_target, g_free);
    g_clear_pointer(&fixture->last_body, g_free);

    if (fixture->dir != NULL)
        clawt_test_remove_tree(fixture->dir);

    g_clear_pointer(&fixture->dir, g_free);
}

static JsonNode *
call_tool(Fixture *fixture, const gchar *agent_id, const gchar *tool_name,
          const gchar *arguments_json)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autofree gchar *request = NULL;
    g_autoptr(GError) error = NULL;

    request = g_strdup_printf(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"%s\",\"arguments\":%s}}",
        tool_name, arguments_json != NULL ? arguments_json : "{}");

    g_assert_true(json_parser_load_from_data(parser, request, -1, &error));

    return clawt_mcp_tools_call(fixture->tools, agent_id,
                                json_parser_get_root(parser));
}

static const gchar *
response_text(JsonNode *response, gboolean *out_is_error)
{
    JsonObject *root = json_node_get_object(response);
    JsonObject *result = json_object_get_object_member(root, "result");
    JsonArray *content = json_object_get_array_member(result, "content");
    JsonObject *first = json_array_get_object_element(content, 0);

    if (out_is_error != NULL)
        *out_is_error = json_object_get_boolean_member(result, "isError");

    return json_object_get_string_member(first, "text");
}

/* ── Listing and permissions ─────────────────────────────────────── */

/*
 * A tool an agent cannot use is omitted, not listed and refused.  An agent
 * that can see a tool will try it, and the refusal costs a turn to discover
 * what it could have been told up front.
 */
static void
test_tools_without_a_computer_are_omitted(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) listing = NULL;
    JsonArray *tools;
    guint i;
    gboolean saw_exec = FALSE;
    gboolean saw_list_agents = FALSE;

    fixture_setup(&fixture, "agents:\n  - id: chief\n");

    listing = clawt_mcp_tools_list(fixture.tools, "chief");
    tools = json_object_get_array_member(json_node_get_object(listing),
                                         "tools");

    for (i = 0; i < json_array_get_length(tools); i++) {
        JsonObject *tool = json_array_get_object_element(tools, i);
        const gchar *name = json_object_get_string_member(tool, "name");

        if (g_strcmp0(name, "clawtilla_computer_exec") == 0)
            saw_exec = TRUE;
        if (g_strcmp0(name, "clawtilla_list_agents") == 0)
            saw_list_agents = TRUE;
    }

    g_assert_false(saw_exec);
    g_assert_true(saw_list_agents);

    fixture_teardown(&fixture);
}

/* Every listed tool carries a schema, or a model has to guess its arguments. */
static void
test_every_listed_tool_has_a_schema(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) listing = NULL;
    JsonArray *tools;
    guint i;

    fixture_setup(&fixture, "agents:\n  - id: chief\n");

    listing = clawt_mcp_tools_list(fixture.tools, "chief");
    tools = json_object_get_array_member(json_node_get_object(listing),
                                         "tools");

    g_assert_cmpuint(json_array_get_length(tools), >, 0);

    for (i = 0; i < json_array_get_length(tools); i++) {
        JsonObject *tool = json_array_get_object_element(tools, i);
        JsonObject *schema;

        g_assert_true(json_object_has_member(tool, "description"));
        g_assert_true(json_object_has_member(tool, "inputSchema"));

        schema = json_object_get_object_member(tool, "inputSchema");
        g_assert_cmpstr(json_object_get_string_member(schema, "type"),
                        ==, "object");

        /* Present even when empty, or a model cannot tell "nothing is
         * required" from "the schema is incomplete". */
        g_assert_true(json_object_has_member(schema, "required"));
    }

    fixture_teardown(&fixture);
}

/* A deny list wins over a broad allow. */
static void
test_deny_beats_allow(void)
{
    Fixture fixture = { 0 };

    fixture_setup(&fixture,
        "agents:\n"
        "  - id: chief\n"
        "    tools:\n"
        "      deny:\n"
        "        - clawtilla_delegate\n");

    g_assert_true(clawt_mcp_tools_is_permitted(fixture.tools, "chief",
                                               "clawtilla_list_agents"));
    g_assert_false(clawt_mcp_tools_is_permitted(fixture.tools, "chief",
                                                "clawtilla_delegate"));

    fixture_teardown(&fixture);
}

/* An allow list narrows to exactly what it names. */
static void
test_allow_list_narrows(void)
{
    Fixture fixture = { 0 };

    fixture_setup(&fixture,
        "agents:\n"
        "  - id: worker\n"
        "    tools:\n"
        "      allow:\n"
        "        - clawtilla_list_agents\n");

    g_assert_true(clawt_mcp_tools_is_permitted(fixture.tools, "worker",
                                               "clawtilla_list_agents"));
    g_assert_false(clawt_mcp_tools_is_permitted(fixture.tools, "worker",
                                                "clawtilla_delegate"));

    fixture_teardown(&fixture);
}

static void
test_unknown_tool_is_refused(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) response = NULL;
    gboolean is_error = FALSE;

    fixture_setup(&fixture, "agents:\n  - id: chief\n");

    response = call_tool(&fixture, "chief", "clawtilla_invent_something",
                         NULL);
    response_text(response, &is_error);
    g_assert_true(is_error);

    fixture_teardown(&fixture);
}

/* ── Listing agents ──────────────────────────────────────────────── */

/* An agent listing itself invites self-delegation. */
static void
test_list_agents_omits_the_caller(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) response = NULL;
    const gchar *text;

    fixture_setup(&fixture,
        "agents:\n"
        "  - id: chief\n"
        "    description: \"hands out work\"\n"
        "  - id: researcher\n"
        "    description: \"reads things\"\n");

    response = call_tool(&fixture, "chief", "clawtilla_list_agents", NULL);
    text = response_text(response, NULL);

    g_assert_nonnull(strstr(text, "researcher"));
    g_assert_nonnull(strstr(text, "reads things"));
    g_assert_null(strstr(text, "hands out work"));

    fixture_teardown(&fixture);
}

static void
test_get_agent_reports_capabilities(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) response = NULL;
    const gchar *text;

    fixture_setup(&fixture,
        "agents:\n"
        "  - id: chief\n"
        "  - id: researcher\n"
        "    description: \"reads things\"\n");

    response = call_tool(&fixture, "chief", "clawtilla_get_agent",
                         "{\"agent_id\":\"researcher\"}");
    text = response_text(response, NULL);

    g_assert_nonnull(strstr(text, "researcher"));
    g_assert_nonnull(strstr(text, "reads things"));
    g_assert_nonnull(strstr(text, "Can:"));

    fixture_teardown(&fixture);
}

/* Asking about an agent that is not there says so by name. */
static void
test_get_unknown_agent_says_so(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) response = NULL;
    gboolean is_error = FALSE;
    const gchar *text;

    fixture_setup(&fixture, "agents:\n  - id: chief\n");

    response = call_tool(&fixture, "chief", "clawtilla_get_agent",
                         "{\"agent_id\":\"nobody\"}");
    text = response_text(response, &is_error);

    g_assert_true(is_error);
    g_assert_nonnull(strstr(text, "nobody"));

    fixture_teardown(&fixture);
}

/* ── Messaging and delegation ────────────────────────────────────── */

static void
test_message_agent_routes(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) response = NULL;
    gboolean is_error = TRUE;

    fixture_setup(&fixture,
        "agents:\n  - id: chief\n  - id: researcher\n");

    response = call_tool(&fixture, "chief", "clawtilla_message_agent",
                         "{\"agent_id\":\"researcher\","
                         "\"body\":\"have a look at the commits\"}");
    response_text(response, &is_error);

    g_assert_false(is_error);
    g_assert_cmpstr(fixture.last_target, ==, "researcher");
    g_assert_cmpstr(fixture.last_body, ==, "have a look at the commits");

    fixture_teardown(&fixture);
}

/* Missing arguments are named, so the model fixes the call rather than
 * guessing at what went wrong. */
static void
test_missing_arguments_are_named(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) response = NULL;
    gboolean is_error = FALSE;
    const gchar *text;

    fixture_setup(&fixture,
        "agents:\n  - id: chief\n  - id: researcher\n");

    response = call_tool(&fixture, "chief", "clawtilla_message_agent",
                         "{\"agent_id\":\"researcher\"}");
    text = response_text(response, &is_error);

    g_assert_true(is_error);
    g_assert_nonnull(strstr(text, "body"));

    fixture_teardown(&fixture);
}

static void
test_delegate_creates_a_task(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) response = NULL;
    g_autoptr(GPtrArray) tasks = NULL;
    gboolean is_error = TRUE;
    const gchar *text;

    fixture_setup(&fixture,
        "agents:\n  - id: chief\n  - id: researcher\n");

    response = call_tool(&fixture, "chief", "clawtilla_delegate",
                         "{\"agent_id\":\"researcher\","
                         "\"task\":\"summarise the week\"}");
    text = response_text(response, &is_error);

    g_assert_false(is_error);
    g_assert_nonnull(strstr(text, "task-"));

    tasks = clawt_task_manager_list(fixture.tasks, "researcher", TRUE);
    g_assert_cmpuint(tasks->len, ==, 1);

    fixture_teardown(&fixture);
}

/* Delegating to somebody who is not there points at the tool that lists
 * who is. */
static void
test_delegate_to_unknown_agent_suggests_listing(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) response = NULL;
    gboolean is_error = FALSE;
    const gchar *text;

    fixture_setup(&fixture, "agents:\n  - id: chief\n");

    response = call_tool(&fixture, "chief", "clawtilla_delegate",
                         "{\"agent_id\":\"ghost\",\"task\":\"do a thing\"}");
    text = response_text(response, &is_error);

    g_assert_true(is_error);
    g_assert_nonnull(strstr(text, "clawtilla_list_agents"));

    fixture_teardown(&fixture);
}

/*
 * A task whose message could not be delivered is failed, not left pending.
 * A task nobody was told about would sit in the list for ever looking like
 * work in progress.
 */
static void
test_undeliverable_delegation_fails_its_task(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) response = NULL;
    g_autoptr(GPtrArray) live = NULL;
    g_autoptr(GPtrArray) all = NULL;
    gboolean is_error = FALSE;

    fixture_setup(&fixture,
        "agents:\n  - id: chief\n  - id: researcher\n");

    fixture.deliver_fails = TRUE;

    response = call_tool(&fixture, "chief", "clawtilla_delegate",
                         "{\"agent_id\":\"researcher\","
                         "\"task\":\"something\"}");
    response_text(response, &is_error);
    g_assert_true(is_error);

    live = clawt_task_manager_list(fixture.tasks, NULL, FALSE);
    all = clawt_task_manager_list(fixture.tasks, NULL, TRUE);

    g_assert_cmpuint(live->len, ==, 0);
    g_assert_cmpuint(all->len, ==, 1);
    g_assert_cmpint(clawt_task_get_state(g_ptr_array_index(all, 0)), ==,
                    CLAWT_TASK_FAILED);

    fixture_teardown(&fixture);
}

/* ── Tasks ───────────────────────────────────────────────────────── */

static void
test_task_status_and_result(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) delegate_response = NULL;
    ClawtTask *task;
    g_autofree gchar *status_args = NULL;

    fixture_setup(&fixture,
        "agents:\n  - id: chief\n  - id: researcher\n");

    delegate_response = call_tool(&fixture, "chief", "clawtilla_delegate",
                                  "{\"agent_id\":\"researcher\","
                                  "\"task\":\"summarise\"}");

    {
        g_autoptr(GPtrArray) tasks =
            clawt_task_manager_list(fixture.tasks, NULL, TRUE);

        task = g_ptr_array_index(tasks, 0);
    }

    status_args = g_strdup_printf("{\"task_id\":\"%s\"}",
                                  clawt_task_get_id(task));

    /* Before it finishes, the result says so rather than being empty. */
    {
        g_autoptr(JsonNode) response =
            call_tool(&fixture, "chief", "clawtilla_task_result", status_args);
        const gchar *text = response_text(response, NULL);

        g_assert_nonnull(strstr(text, "no result yet"));
    }

    clawt_task_manager_complete(fixture.tasks, clawt_task_get_id(task),
                                "here is the summary");

    {
        g_autoptr(JsonNode) response =
            call_tool(&fixture, "chief", "clawtilla_task_result", status_args);
        const gchar *text = response_text(response, NULL);

        g_assert_cmpstr(text, ==, "here is the summary");
    }

    fixture_teardown(&fixture);
}

/*
 * An assignee reporting completion is how the delegator learns the work is
 * done; only replying leaves them waiting.
 */
static void
test_assignee_can_complete_its_task(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) delegate_response = NULL;
    g_autoptr(JsonNode) complete_response = NULL;
    g_autofree gchar *args = NULL;
    ClawtTask *task;
    gboolean is_error = TRUE;

    fixture_setup(&fixture,
        "agents:\n  - id: chief\n  - id: researcher\n");

    delegate_response = call_tool(&fixture, "chief", "clawtilla_delegate",
                                  "{\"agent_id\":\"researcher\","
                                  "\"task\":\"summarise\"}");

    {
        g_autoptr(GPtrArray) tasks =
            clawt_task_manager_list(fixture.tasks, NULL, TRUE);

        task = g_ptr_array_index(tasks, 0);
    }

    args = g_strdup_printf("{\"task_id\":\"%s\",\"result\":\"done\"}",
                           clawt_task_get_id(task));

    complete_response = call_tool(&fixture, "researcher",
                                  "clawtilla_task_complete", args);
    response_text(complete_response, &is_error);

    g_assert_false(is_error);
    g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_COMPLETED);

    fixture_teardown(&fixture);
}

/*
 * An agent can read its conversation with a peer by naming the peer.
 *
 * Asked to "message test and see what they say", an agent had nowhere
 * to look: its mailbox is empty because delivery drains it, and reading
 * the exchange meant knowing to type "dm:<sorted>:<pair>" -- the
 * internal naming a caller is specifically told not to depend on. It
 * reported that nothing had come back while the reply sat in the
 * transcript.
 */
static void
test_room_history_takes_an_agent_id(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) response = NULL;
    ClawtRoom *room;
    const gchar *text;

    fixture_setup(&fixture, "agents:\n  - id: chief\n  - id: researcher\n");

    room = clawt_room_manager_get_direct(fixture.rooms, "chief",
                                         "researcher");
    {
        g_autoptr(ClawtMessage) said =
            clawt_message_new(clawt_room_get_id(room), "researcher",
                              "the build broke on the lease sweep");

        g_assert_true(clawt_room_append(room, said, NULL));
    }

    response = call_tool(&fixture, "chief", "clawtilla_room_history",
                         "{\"room_id\":\"researcher\"}");
    text = response_text(response, NULL);

    g_assert_nonnull(strstr(text, "the build broke on the lease sweep"));
    g_assert_nonnull(strstr(text, "researcher"));

    fixture_teardown(&fixture);
}

/* ── Mailbox ─────────────────────────────────────────────────────── */

static void
test_mailbox_list_reports_waiting_messages(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) empty_response = NULL;
    g_autoptr(JsonNode) response = NULL;
    ClawtAgent *agent;
    const gchar *text;

    fixture_setup(&fixture, "agents:\n  - id: chief\n");

    empty_response = call_tool(&fixture, "chief", "clawtilla_mailbox_list",
                               NULL);
    text = response_text(empty_response, NULL);
    g_assert_nonnull(strstr(text, "empty"));

    /*
     * And says why, because while an agent is running its mailbox is
     * almost always empty and that fact means nothing. One that checked
     * here to find out whether a peer had answered concluded, correctly
     * and uselessly, that nothing had.
     */
    g_assert_nonnull(strstr(text, "clawtilla_room_history"));

    agent = clawt_agent_manager_get(fixture.agents, "chief");

    {
        g_autoptr(ClawtMailboxItem) item =
            clawt_mailbox_item_new("researcher", "chief", "the summary");
        g_autofree gchar *id = NULL;

        id = clawt_mailbox_post(clawt_agent_get_mailbox(agent), item, NULL);
        g_assert_nonnull(id);
    }

    response = call_tool(&fixture, "chief", "clawtilla_mailbox_list", NULL);
    text = response_text(response, NULL);

    g_assert_nonnull(strstr(text, "the summary"));
    g_assert_nonnull(strstr(text, "researcher"));

    fixture_teardown(&fixture);
}

/* ── Computer ────────────────────────────────────────────────────── */

static void
test_computer_exec_through_the_tool(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtSandbox) sandbox = NULL;
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(JsonNode) response = NULL;
    ClawtAgent *agent;
    gboolean is_error = TRUE;
    const gchar *text;

    fixture_setup(&fixture, "agents:\n  - id: chief\n");

    agent = clawt_agent_manager_get(fixture.agents, "chief");
    sandbox = clawt_sandbox_new(CLAWT_CONFINE_WORKSPACE, fixture.dir);
    computer = clawt_host_computer_new("chief", sandbox);
    clawt_computer_start(computer, NULL);
    clawt_agent_set_computer(agent, computer);

    g_assert_true(clawt_mcp_tools_is_permitted(fixture.tools, "chief",
                                               "clawtilla_computer_exec"));

    response = call_tool(&fixture, "chief", "clawtilla_computer_exec",
                         "{\"command\":\"echo hello\",\"timeout\":10}");
    text = response_text(response, &is_error);

    g_assert_false(is_error);
    g_assert_nonnull(strstr(text, "hello"));

    fixture_teardown(&fixture);
}

/*
 * A failing command must report its exit status and stderr.  A reply that
 * is only stdout tells the agent nothing about why it failed.
 */
static void
test_failing_command_reports_why(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtSandbox) sandbox = NULL;
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(JsonNode) response = NULL;
    ClawtAgent *agent;
    gboolean is_error = FALSE;
    const gchar *text;

    fixture_setup(&fixture, "agents:\n  - id: chief\n");

    agent = clawt_agent_manager_get(fixture.agents, "chief");
    sandbox = clawt_sandbox_new(CLAWT_CONFINE_NONE, fixture.dir);
    computer = clawt_host_computer_new("chief", sandbox);
    clawt_computer_start(computer, NULL);
    clawt_agent_set_computer(agent, computer);

    response = call_tool(&fixture, "chief", "clawtilla_computer_exec",
                         "{\"command\":\"sh -c 'echo nope >&2; exit 7'\","
                         "\"timeout\":10}");
    text = response_text(response, &is_error);

    g_assert_true(is_error);
    g_assert_nonnull(strstr(text, "7"));
    g_assert_nonnull(strstr(text, "nope"));

    fixture_teardown(&fixture);
}


/* ── Growing the fleet ───────────────────────────────────────────── */

typedef struct {
    gchar      *created_id;
    GHashTable *created_settings;
    gboolean    started;
    gboolean    refuse;
} FleetRecord;

static gchar *
fake_create_agent(const gchar  *agent_id,
                  GHashTable   *settings,
                  gboolean      start,
                  gpointer      user_data,
                  GError      **error)
{
    FleetRecord *record = user_data;

    if (record->refuse) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "a VM needs a disk image to boot");
        return NULL;
    }

    g_free(record->created_id);
    record->created_id = g_strdup(agent_id);
    record->started = start;

    g_clear_pointer(&record->created_settings, g_hash_table_unref);
    record->created_settings = g_hash_table_ref(settings);

    return g_strdup_printf("Created %s.", agent_id);
}

static gboolean
offers_tool(Fixture *fixture, const gchar *agent_id, const gchar *tool_name)
{
    g_autoptr(JsonNode) listed = clawt_mcp_tools_list(fixture->tools,
                                                      agent_id);
    JsonArray *tools =
        json_object_get_array_member(json_node_get_object(listed), "tools");
    guint i;

    for (i = 0; i < json_array_get_length(tools); i++) {
        JsonObject *tool = json_array_get_object_element(tools, i);

        if (g_strcmp0(json_object_get_string_member(tool, "name"),
                      tool_name) == 0)
            return TRUE;
    }

    return FALSE;
}

/*
 * Creating agents is off unless it was granted, and a tool that is not
 * granted is not *offered*.
 *
 * Listing it and refusing the call costs the agent a turn to learn
 * something it could have been told, and teaches it to keep trying.
 */
static void
test_the_fleet_tools_need_the_permission(void)
{
    Fixture fixture = { 0 };
    FleetRecord record = { 0 };

    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: chief\n"
                  "    tools:\n"
                  "      manage_fleet: true\n"
                  "  - id: worker\n");

    clawt_mcp_tools_set_create_agent_func(fixture.tools, fake_create_agent,
                                          &record, NULL);

    g_assert_true(offers_tool(&fixture, "chief", "clawtilla_create_agent"));
    g_assert_true(offers_tool(&fixture, "chief", "clawtilla_agent_options"));

    g_assert_false(offers_tool(&fixture, "worker",
                               "clawtilla_create_agent"));
    g_assert_false(offers_tool(&fixture, "worker",
                               "clawtilla_agent_options"));

    g_clear_pointer(&record.created_id, g_free);
    g_clear_pointer(&record.created_settings, g_hash_table_unref);
    fixture_teardown(&fixture);
}

/*
 * ...and not offered at all when nothing can create one, whatever the
 * permission says.  A library embedded without a daemon has no fleet to
 * add to, and the failure would arrive as a confident call.
 */
static void
test_no_fleet_tools_without_somewhere_to_create(void)
{
    Fixture fixture = { 0 };

    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: chief\n"
                  "    tools:\n"
                  "      manage_fleet: true\n");

    g_assert_false(offers_tool(&fixture, "chief", "clawtilla_create_agent"));

    fixture_teardown(&fixture);
}

/*
 * The named arguments and the free-form ones end in the same place, so
 * every option in the schema is reachable without a list here that would
 * drift from it the first time somebody added a key.
 */
static void
test_creating_an_agent_passes_every_setting_through(void)
{
    Fixture fixture = { 0 };
    FleetRecord record = { 0 };
    g_autoptr(JsonNode) response = NULL;
    gboolean is_error = TRUE;

    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: chief\n"
                  "    tools:\n"
                  "      manage_fleet: true\n");

    clawt_mcp_tools_set_create_agent_func(fixture.tools, fake_create_agent,
                                          &record, NULL);

    response = call_tool(&fixture, "chief", "clawtilla_create_agent",
        "{\"agent_id\":\"scribe\","
        "\"description\":\"writes things down\","
        "\"purpose\":\"You keep the notes.\","
        "\"computer\":\"container\","
        "\"container_image\":\"fedora:latest\","
        "\"settings\":\"computer.vm.resolution=1920x1080\\n"
        "# a comment\\n\\nmodel.effort=high\"}");

    response_text(response, &is_error);
    g_assert_false(is_error);

    g_assert_cmpstr(record.created_id, ==, "scribe");
    g_assert_true(record.started);

    g_assert_cmpstr(g_hash_table_lookup(record.created_settings,
                                        "description"),
                    ==, "writes things down");
    g_assert_cmpstr(g_hash_table_lookup(record.created_settings, "persona"),
                    ==, "You keep the notes.");
    g_assert_cmpstr(g_hash_table_lookup(record.created_settings,
                                        "computer.type"),
                    ==, "container");
    g_assert_cmpstr(g_hash_table_lookup(record.created_settings,
                                        "computer.container.image"),
                    ==, "fedora:latest");

    /* ...including the ones nothing here has heard of. */
    g_assert_cmpstr(g_hash_table_lookup(record.created_settings,
                                        "computer.vm.resolution"),
                    ==, "1920x1080");
    g_assert_cmpstr(g_hash_table_lookup(record.created_settings,
                                        "model.effort"),
                    ==, "high");

    g_clear_pointer(&record.created_id, g_free);
    g_clear_pointer(&record.created_settings, g_hash_table_unref);
    fixture_teardown(&fixture);
}

/*
 * A refusal reaches the agent as the reason, not as a bare failure --
 * and it has to say the agent was not created, or the agent tries again
 * with the same arguments.
 */
static void
test_a_refused_creation_says_why(void)
{
    Fixture fixture = { 0 };
    FleetRecord record = { 0 };
    g_autoptr(JsonNode) response = NULL;
    const gchar *text;
    gboolean is_error = FALSE;

    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: chief\n"
                  "    tools:\n"
                  "      manage_fleet: true\n");

    record.refuse = TRUE;
    clawt_mcp_tools_set_create_agent_func(fixture.tools, fake_create_agent,
                                          &record, NULL);

    response = call_tool(&fixture, "chief", "clawtilla_create_agent",
        "{\"agent_id\":\"vmbox\",\"description\":\"x\","
        "\"computer\":\"vm\"}");

    text = response_text(response, &is_error);

    g_assert_true(is_error);
    g_assert_nonnull(strstr(text, "disk image"));

    g_clear_pointer(&record.created_id, g_free);
    g_clear_pointer(&record.created_settings, g_hash_table_unref);
    fixture_teardown(&fixture);
}

/*
 * And an id or a description that was left out is refused here rather
 * than becoming an agent nobody can identify the purpose of.
 */
static void
test_a_new_agent_needs_an_id_and_a_description(void)
{
    Fixture fixture = { 0 };
    FleetRecord record = { 0 };
    g_autoptr(JsonNode) without_id = NULL;
    g_autoptr(JsonNode) without_description = NULL;
    gboolean is_error = FALSE;

    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: chief\n"
                  "    tools:\n"
                  "      manage_fleet: true\n");

    clawt_mcp_tools_set_create_agent_func(fixture.tools, fake_create_agent,
                                          &record, NULL);

    without_id = call_tool(&fixture, "chief", "clawtilla_create_agent",
                           "{\"description\":\"x\"}");
    response_text(without_id, &is_error);
    g_assert_true(is_error);

    is_error = FALSE;
    without_description = call_tool(&fixture, "chief",
                                    "clawtilla_create_agent",
                                    "{\"agent_id\":\"scribe\"}");
    response_text(without_description, &is_error);
    g_assert_true(is_error);

    g_assert_null(record.created_id);

    g_clear_pointer(&record.created_settings, g_hash_table_unref);
    fixture_teardown(&fixture);
}

/*
 * The options tool reports what exists.  An agent asked to create
 * another will otherwise invent a provider and a disk image path, and
 * both produce something that looks created and does not work.
 */
static void
test_the_options_report_what_can_be_chosen(void)
{
    Fixture fixture = { 0 };
    FleetRecord record = { 0 };
    g_autoptr(JsonNode) response = NULL;
    const gchar *text;
    gboolean is_error = TRUE;

    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: chief\n"
                  "    tools:\n"
                  "      manage_fleet: true\n");

    clawt_mcp_tools_set_create_agent_func(fixture.tools, fake_create_agent,
                                          &record, NULL);

    response = call_tool(&fixture, "chief", "clawtilla_agent_options", "{}");
    text = response_text(response, &is_error);

    g_assert_false(is_error);
    g_assert_nonnull(strstr(text, "claude-code"));
    g_assert_nonnull(strstr(text, "container"));

    /* Settable keys come from the schema, so they cannot drift from it. */
    g_assert_nonnull(strstr(text, "computer.vm.resolution"));

    /*
     * And with no image store it says it cannot tell, rather than
     * offering an empty list that reads as "there are none".
     */
    g_assert_nonnull(strstr(text, "unknown from here"));

    g_clear_pointer(&record.created_id, g_free);
    g_clear_pointer(&record.created_settings, g_hash_table_unref);
    fixture_teardown(&fixture);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/mcp/fleet/needs-the-permission",
                    test_the_fleet_tools_need_the_permission);
    g_test_add_func("/mcp/fleet/none-without-a-daemon",
                    test_no_fleet_tools_without_somewhere_to_create);
    g_test_add_func("/mcp/fleet/every-setting-passes-through",
                    test_creating_an_agent_passes_every_setting_through);
    g_test_add_func("/mcp/fleet/refusal-says-why",
                    test_a_refused_creation_says_why);
    g_test_add_func("/mcp/fleet/id-and-description-required",
                    test_a_new_agent_needs_an_id_and_a_description);
    g_test_add_func("/mcp/fleet/options-report-what-exists",
                    test_the_options_report_what_can_be_chosen);
    g_test_add_func("/mcp/omits-unusable-tools",
                    test_tools_without_a_computer_are_omitted);
    g_test_add_func("/mcp/schemas", test_every_listed_tool_has_a_schema);
    g_test_add_func("/mcp/deny-beats-allow", test_deny_beats_allow);
    g_test_add_func("/mcp/allow-narrows", test_allow_list_narrows);
    g_test_add_func("/mcp/unknown-tool", test_unknown_tool_is_refused);

    g_test_add_func("/mcp/list-omits-caller", test_list_agents_omits_the_caller);
    g_test_add_func("/mcp/get-agent", test_get_agent_reports_capabilities);
    g_test_add_func("/mcp/get-unknown-agent", test_get_unknown_agent_says_so);

    g_test_add_func("/mcp/message-routes", test_message_agent_routes);
    g_test_add_func("/mcp/missing-arguments", test_missing_arguments_are_named);
    g_test_add_func("/mcp/delegate", test_delegate_creates_a_task);
    g_test_add_func("/mcp/delegate-unknown",
                    test_delegate_to_unknown_agent_suggests_listing);
    g_test_add_func("/mcp/undeliverable-delegation",
                    test_undeliverable_delegation_fails_its_task);

    g_test_add_func("/mcp/task-status-result", test_task_status_and_result);
    g_test_add_func("/mcp/assignee-completes",
                    test_assignee_can_complete_its_task);

    g_test_add_func("/mcp/mailbox-list", test_mailbox_list_reports_waiting_messages);
    g_test_add_func("/mcp/room-history-by-agent",
                    test_room_history_takes_an_agent_id);

    g_test_add_func("/mcp/computer-exec", test_computer_exec_through_the_tool);
    g_test_add_func("/mcp/failing-command", test_failing_command_reports_why);

    return g_test_run();
}
