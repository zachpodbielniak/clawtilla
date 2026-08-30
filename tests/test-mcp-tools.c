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
    ClawtEventBus     *bus;
    ClawtMcpTools     *tools;

    gchar             *last_target;
    gchar             *last_body;
    ClawtPriority      last_priority;
    gint               last_depth;
    gboolean           deliver_fails;
} Fixture;

static gboolean
fake_deliver(const gchar   *from_agent,
             const gchar   *target,
             const gchar   *body,
             const gchar   *task_id,
             gint           depth,
             ClawtPriority  priority,
             gpointer       user_data,
             GError       **error)
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
    fixture->last_priority = priority;
    fixture->last_depth = depth;

    return TRUE;
}

static void
fixture_setup(Fixture *fixture, const gchar *agents_yaml)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *yaml = NULL;

    fixture->dir = g_dir_make_tmp("clawt-mcp-XXXXXX", NULL);

    /*
     * workspace_root as well as state_dir.  Three things escape a
     * temporary directory unless a fixture names them, and this one
     * named only the second: defaults.workspace_root falls back to
     * ~/.clawtilla/agents, so any test here that scaffolds a workspace
     * writes into the developer's real fleet, where the leftovers are
     * indistinguishable from agents somebody meant to keep.  Nothing on
     * this path scaffolds one today, which is exactly how it went
     * unnoticed -- it is one call away from doing so.
     */
    yaml = g_strdup_printf("daemon:\n  state_dir: \"%s\"\n"
                           "defaults:\n  workspace_root: \"%s/agents\"\n%s",
                           fixture->dir, fixture->dir, agents_yaml);

    fixture->config = clawt_config_load_from_string(yaml, &error);
    g_assert_no_error(error);

    fixture->agents = clawt_agent_manager_new(fixture->config);
    clawt_agent_manager_load(fixture->agents, NULL);

    fixture->tasks = clawt_task_manager_new();
    fixture->guard = clawt_loop_guard_new();
    fixture->rooms = clawt_room_manager_new(NULL);
    fixture->bus = clawt_event_bus_new(64);
    fixture->tools = clawt_mcp_tools_new(fixture->agents, fixture->tasks,
                                         fixture->guard);
    clawt_mcp_tools_set_room_manager(fixture->tools, fixture->rooms);
    clawt_mcp_tools_set_event_bus(fixture->tools, fixture->bus);

    clawt_mcp_tools_set_deliver_func(fixture->tools, fake_deliver, fixture,
                                     NULL);
}

static void
fixture_teardown(Fixture *fixture)
{
    g_clear_object(&fixture->tools);
    g_clear_object(&fixture->bus);
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

/*
 * As call_tool(), naming which of the agent's conversations the call is
 * part of -- which is what clawtilla-mcp-server passes from the
 * environment libreclaw set on the CLI it was spawned by.
 */
static JsonNode *
call_tool_in(Fixture *fixture, const gchar *agent_id, const gchar *room_id,
             const gchar *tool_name, const gchar *arguments_json)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autofree gchar *request = NULL;
    g_autoptr(GError) error = NULL;

    request = g_strdup_printf(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"%s\",\"arguments\":%s}}",
        tool_name, arguments_json != NULL ? arguments_json : "{}");

    g_assert_true(json_parser_load_from_data(parser, request, -1, &error));

    return clawt_mcp_tools_call(fixture->tools, agent_id, room_id,
                                json_parser_get_root(parser));
}

static JsonNode *
call_tool(Fixture *fixture, const gchar *agent_id, const gchar *tool_name,
          const gchar *arguments_json)
{
    return call_tool_in(fixture, agent_id, NULL, tool_name, arguments_json);
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
        "agents:\n  - id: chief\n    chief_of_staff: true\n"
        "  - id: researcher\n");

    response = call_tool(&fixture, "chief", "clawtilla_message_agent",
                         "{\"agent_id\":\"researcher\","
                         "\"body\":\"have a look at the commits\"}");
    response_text(response, &is_error);

    g_assert_false(is_error);
    g_assert_cmpstr(fixture.last_target, ==, "researcher");
    g_assert_cmpstr(fixture.last_body, ==, "have a look at the commits");

    fixture_teardown(&fixture);
}

/*
 * The band an agent names reaches the thing that routes the message.
 *
 * `priority` has been in this tool's schema, and its description has
 * promised that "urgent jumps the queue", for the whole life of the
 * feature -- while the band was parsed into a local and dropped:
 * ClawtMcpDeliverFunc had no band to carry it on.  So every message
 * every agent had ever sent was queued at NORMAL, and the promise was
 * true of nothing.
 *
 * Asserted on what the deliver hook was *handed*, not on the reply
 * text: a reply saying "queued as urgent" is exactly the sentence that
 * was worth nothing before this, and a test that believed it would have
 * passed throughout.
 */
static void
test_message_agent_carries_the_priority(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) response = NULL;
    gboolean is_error = TRUE;

    fixture_setup(&fixture,
        "agents:\n  - id: chief\n    chief_of_staff: true\n"
        "  - id: researcher\n");

    fixture.last_priority = CLAWT_PRIORITY_LOW;

    response = call_tool(&fixture, "chief", "clawtilla_message_agent",
                         "{\"agent_id\":\"researcher\","
                         "\"body\":\"the build is broken\","
                         "\"priority\":\"urgent\"}");
    response_text(response, &is_error);

    g_assert_false(is_error);
    g_assert_cmpint(fixture.last_priority, ==, CLAWT_PRIORITY_URGENT);

    fixture_teardown(&fixture);
}

/*
 * And a message that names no band is NORMAL rather than zero.
 *
 * CLAWT_PRIORITY_LOW is 0, so anything that reaches the queue through a
 * zeroed field posts at the band `drop-oldest` sheds *first*.  Every
 * ordinary message goes down this path, which makes it the one worth
 * pinning: a regression here would demote the whole fleet's traffic
 * while every test about urgency still passed.
 */
static void
test_a_message_with_no_band_is_normal(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) response = NULL;
    gboolean is_error = TRUE;

    fixture_setup(&fixture,
        "agents:\n  - id: chief\n    chief_of_staff: true\n"
        "  - id: researcher\n");

    fixture.last_priority = CLAWT_PRIORITY_URGENT;

    response = call_tool(&fixture, "chief", "clawtilla_message_agent",
                         "{\"agent_id\":\"researcher\","
                         "\"body\":\"whenever you get a moment\"}");
    response_text(response, &is_error);

    g_assert_false(is_error);
    g_assert_cmpint(fixture.last_priority, ==, CLAWT_PRIORITY_NORMAL);

    fixture_teardown(&fixture);
}

/*
 * A band nobody has heard of is refused, and nothing is sent.
 *
 * The alternative -- falling through to a zeroed ClawtPriority -- is
 * LOW, so a model that wrote "critical" or "P1" would have its one
 * message that could not wait queued at the band shed first, and
 * believe it had escalated.  Asserted on the delivery having not
 * happened as well as on the refusal: a refusal that arrives *after*
 * the message has gone leaves the agent with no remedy but to send it
 * twice.
 */
static void
test_an_unknown_band_is_refused_before_anything_is_sent(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) response = NULL;
    const gchar *text;
    gboolean is_error = FALSE;

    fixture_setup(&fixture,
        "agents:\n  - id: chief\n    chief_of_staff: true\n"
        "  - id: researcher\n");

    response = call_tool(&fixture, "chief", "clawtilla_message_agent",
                         "{\"agent_id\":\"researcher\","
                         "\"body\":\"the build is broken\","
                         "\"priority\":\"P1\"}");
    text = response_text(response, &is_error);

    g_assert_true(is_error);
    g_assert_nonnull(strstr(text, "urgent"));
    g_assert_null(fixture.last_target);

    fixture_teardown(&fixture);
}

/*
 * clawtilla_ask_agent queues and returns, exactly like
 * clawtilla_message_agent.
 *
 * This is the behaviour the tool's description states and the workspace
 * templates used to contradict, so it is worth pinning: the reply is
 * "Queued for ...", the question has been delivered, and nothing in this
 * call ever carries the other agent's answer back.  An agent told
 * otherwise waits for something that is not coming.
 */
static void
test_ask_agent_queues_rather_than_waiting(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) response = NULL;
    gboolean is_error = TRUE;
    const gchar *text;

    fixture_setup(&fixture,
        "agents:\n  - id: chief\n    chief_of_staff: true\n"
        "  - id: researcher\n");

    response = call_tool(&fixture, "chief", "clawtilla_ask_agent",
                         "{\"agent_id\":\"researcher\","
                         "\"message\":\"did the build pass?\"}");
    text = response_text(response, &is_error);

    g_assert_false(is_error);
    g_assert_cmpstr(fixture.last_target, ==, "researcher");
    g_assert_cmpstr(fixture.last_body, ==, "did the build pass?");

    /* Queued, not answered. */
    g_assert_nonnull(strstr(text, "Queued for researcher"));

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
        "agents:\n  - id: chief\n    chief_of_staff: true\n"
        "  - id: researcher\n");

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

    /*
     * Marked as the chief, because assigning work is now a role rather
     * than something any agent may do. An agent that is neither the
     * chief nor a team lead is refused, which is what the team tests
     * cover.
     */
    fixture_setup(&fixture,
        "agents:\n  - id: chief\n    chief_of_staff: true\n"
        "  - id: researcher\n");

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

    fixture_setup(&fixture,
                  "agents:\n  - id: chief\n    chief_of_staff: true\n");

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
        "agents:\n  - id: chief\n    chief_of_staff: true\n"
        "  - id: researcher\n");

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

/*
 * The gap this tool closes: a chief could ask about a task it had kept
 * the id of, and had no way to ask what it had handed out.  It is the
 * assignee of none of that work, so every listing that existed answered
 * it with nothing.
 */
static void
test_task_list_shows_both_directions(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) out = NULL;
    g_autoptr(JsonNode) response = NULL;
    gboolean is_error = FALSE;
    const gchar *text;

    fixture_setup(&fixture,
        "agents:\n  - id: chief\n    chief_of_staff: true\n"
        "  - id: researcher\n  - id: writer\n");

    out = call_tool(&fixture, "chief", "clawtilla_delegate",
                    "{\"agent_id\":\"researcher\",\"task\":\"read the logs\"}");
    response_text(out, &is_error);
    g_assert_false(is_error);

    /* Work handed to the chief by somebody else. */
    clawt_task_manager_create(fixture.tasks, "writer", "chief",
                              "approve the draft", NULL, NULL);

    response = call_tool(&fixture, "chief", "clawtilla_task_list", "{}");
    text = response_text(response, &is_error);

    g_assert_false(is_error);
    g_assert_nonnull(strstr(text, "Delegated by you"));
    g_assert_nonnull(strstr(text, "researcher"));
    g_assert_nonnull(strstr(text, "read the logs"));
    g_assert_nonnull(strstr(text, "Assigned to you"));
    g_assert_nonnull(strstr(text, "from writer"));
    g_assert_nonnull(strstr(text, "approve the draft"));

    /*
     * The age, not just the shape.  A task stamps itself in seconds and
     * the label takes microseconds, so the first version of this printed
     * "20694d ago" -- the epoch -- on every row, and every assertion
     * above still passed.  Asserting on structure alone cannot see a
     * wrong value.
     */
    g_assert_nonnull(strstr(text, "just now"));
    g_assert_null(strstr(text, "d ago"));

    fixture_teardown(&fixture);
}

/*
 * `pending` is what an agent-delegated task reads for its whole life,
 * because clawtilla_delegate does not mark it running.  A chief that
 * reads that as "never picked up" delegates again and makes two of
 * everything, so the tool says so where the column is read.
 */
static void
test_task_list_explains_pending(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) out = NULL;
    g_autoptr(JsonNode) response = NULL;
    gboolean is_error = FALSE;
    const gchar *text;

    fixture_setup(&fixture,
        "agents:\n  - id: chief\n    chief_of_staff: true\n"
        "  - id: researcher\n");

    out = call_tool(&fixture, "chief", "clawtilla_delegate",
                    "{\"agent_id\":\"researcher\",\"task\":\"a thing\"}");
    response_text(out, &is_error);

    response = call_tool(&fixture, "chief", "clawtilla_task_list", "{}");
    text = response_text(response, &is_error);

    g_assert_false(is_error);
    g_assert_nonnull(strstr(text, "do not delegate it again"));

    fixture_teardown(&fixture);
}

/*
 * An empty list is not "nothing was delegated".  Tasks are held in
 * memory, so a restart clears them -- the same shape as an empty
 * mailbox for a running agent, and it has to say which it is.
 */
static void
test_an_empty_task_list_says_why(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) response = NULL;
    gboolean is_error = FALSE;
    const gchar *text;

    fixture_setup(&fixture,
                  "agents:\n  - id: chief\n    chief_of_staff: true\n");

    response = call_tool(&fixture, "chief", "clawtilla_task_list", "{}");
    text = response_text(response, &is_error);

    g_assert_false(is_error);
    g_assert_nonnull(strstr(text, "held in memory"));
    g_assert_nonnull(strstr(text, "event log"));

    fixture_teardown(&fixture);
}

/*
 * One agent's tasks are not another's.  The listing is scoped to the
 * caller, so a tool with no permission gate still cannot be used to read
 * what the rest of the fleet is doing.
 */
static void
test_task_list_is_scoped_to_the_caller(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) response = NULL;
    gboolean is_error = FALSE;
    const gchar *text;

    fixture_setup(&fixture,
        "agents:\n  - id: chief\n    chief_of_staff: true\n"
        "  - id: researcher\n  - id: writer\n");

    clawt_task_manager_create(fixture.tasks, "researcher", "writer",
                              "nothing to do with the chief", NULL, NULL);

    response = call_tool(&fixture, "chief", "clawtilla_task_list", "{}");
    text = response_text(response, &is_error);

    g_assert_false(is_error);
    g_assert_null(strstr(text, "nothing to do with the chief"));

    fixture_teardown(&fixture);
}

static void
test_task_status_and_result(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) delegate_response = NULL;
    ClawtTask *task;
    g_autofree gchar *status_args = NULL;

    fixture_setup(&fixture,
        "agents:\n  - id: chief\n    chief_of_staff: true\n"
        "  - id: researcher\n");

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
        "agents:\n  - id: chief\n    chief_of_staff: true\n"
        "  - id: researcher\n");

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

    fixture_setup(&fixture, "agents:\n  - id: chief\n    chief_of_staff: true\n"
        "  - id: researcher\n");

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
/*
 * The async form completes on the context the daemon named, not on
 * whichever one happened to be thread-default.
 *
 * clawt_mcp_tools_call_async() creates two GTasks, and g_task_new()
 * captures g_main_context_ref_thread_default() -- which is the *process*
 * default unless somebody pushed one.  Dispatching a source pushes
 * nothing; that is measured, not assumed, and it is the trap already
 * recorded in this tree for the timers, the idle and the autostart task.
 *
 * It works today through a mechanism nothing states: g_task_return_now()
 * pushes the task's own context around the callback it invokes, so the
 * IPC server's async read callback happens to have the daemon's context
 * pushed when it reaches the handler.  Every call into this function
 * inherits that by luck of its caller rather than by anything this
 * object does, and an embedding host driving the same object from an
 * idle of its own -- the case libclawt exists for -- would get the
 * process default and never see the answer at all.
 *
 * So the context is named.  This test calls from a caller with *no*
 * thread-default at all, which is what any ordinary caller looks like,
 * and iterates only the context the tools were given.
 */
typedef struct {
    gboolean  arrived;
    JsonNode *response;
} AsyncProbe;

static void
on_async_exec_done(GObject *source, GAsyncResult *result, gpointer data)
{
    AsyncProbe *probe = data;

    probe->response = clawt_mcp_tools_call_finish(CLAWT_MCP_TOOLS(source),
                                                  result);
    probe->arrived = TRUE;
}

static void
test_an_async_exec_answers_on_the_named_context(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GMainContext) context = g_main_context_new();
    g_autoptr(ClawtSandbox) sandbox = NULL;
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(JsonParser) parser = json_parser_new();
    AsyncProbe probe = { FALSE, NULL };
    ClawtAgent *agent;
    gint64 deadline;
    gboolean is_error = TRUE;
    const gchar *text;

    fixture_setup(&fixture, "agents:\n  - id: chief\n");
    clawt_mcp_tools_set_main_context(fixture.tools, context);

    agent = clawt_agent_manager_get(fixture.agents, "chief");
    sandbox = clawt_sandbox_new(CLAWT_CONFINE_WORKSPACE, fixture.dir);
    computer = clawt_host_computer_new("chief", sandbox);
    clawt_computer_start(computer, NULL);
    clawt_agent_set_computer(agent, computer);

    g_assert_true(json_parser_load_from_data(
        parser,
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"clawtilla_computer_exec\","
        "\"arguments\":{\"command\":\"echo named\",\"timeout\":10}}}",
        -1, NULL));

    /*
     * The positive control, in the same run: this request must be one
     * the daemon would actually defer.  Without it the test would pass
     * against a build where nothing defers at all, since the
     * synchronous path answers from the callback too.
     */
    g_assert_true(clawt_mcp_tools_call_defers(fixture.tools, "chief",
                                              json_parser_get_root(parser)));

    /*
     * No thread-default pushed here on purpose.  This is what a caller
     * that is not already inside a GTask callback looks like.
     */
    g_assert_null(g_main_context_get_thread_default());

    clawt_mcp_tools_call_async(fixture.tools, "chief", NULL,
                               json_parser_get_root(parser),
                               on_async_exec_done, &probe);

    deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;

    while (!probe.arrived && g_get_monotonic_time() < deadline)
        g_main_context_iteration(context, FALSE);

    g_assert_true(probe.arrived);
    g_assert_nonnull(probe.response);

    text = response_text(probe.response, &is_error);
    g_assert_false(is_error);
    g_assert_nonnull(strstr(text, "named"));

    json_node_unref(probe.response);
    fixture_teardown(&fixture);
}



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


/*
 * Finds the one computer.exec event on the bus, or %NULL.
 *
 * Read back through clawt_event_bus_replay() rather than by connecting a
 * signal, because what an operator later looks up is the recorded
 * history rather than the moment it happened.
 */
static ClawtEvent *
recorded_exec(Fixture *fixture)
{
    g_autoptr(GPtrArray) events =
        clawt_event_bus_replay(fixture->bus, 0, NULL);
    ClawtEvent *found = NULL;
    guint i;

    for (i = 0; events != NULL && i < events->len; i++) {
        ClawtEvent *event = g_ptr_array_index(events, i);

        if (g_strcmp0(clawt_event_get_kind(event), "computer.exec") == 0)
            found = event;
    }

    return found;
}

/*
 * An agent running its own command lands on the audit trail.
 *
 * The client `computer.exec` handler has published one of these per
 * command since the daemon was written, and ClawtMcpTools had no route
 * to an event bus at all -- so the same command run by the agent itself
 * was invisible, while docs/security.org asserted both were recorded.
 * The missing half is exactly the one somebody would want to look up:
 * what the fleet did on its own initiative.
 *
 * Asserted on the subject and the two details, because the whole value
 * of the trail is being able to filter it by agent -- an event whose
 * subject were the command, or the computer, would be recorded and
 * useless.
 */
static void
test_an_agents_exec_is_audited(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtSandbox) sandbox = NULL;
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(JsonNode) response = NULL;
    ClawtAgent *agent;
    ClawtEvent *event;

    fixture_setup(&fixture, "agents:\n  - id: chief\n");

    agent = clawt_agent_manager_get(fixture.agents, "chief");
    sandbox = clawt_sandbox_new(CLAWT_CONFINE_WORKSPACE, fixture.dir);
    computer = clawt_host_computer_new("chief", sandbox);
    clawt_computer_start(computer, NULL);
    clawt_agent_set_computer(agent, computer);

    response = call_tool(&fixture, "chief", "clawtilla_computer_exec",
                         "{\"command\":\"echo hello\",\"timeout\":10}");
    g_assert_nonnull(response);

    event = recorded_exec(&fixture);
    g_assert_nonnull(event);
    g_assert_cmpstr(clawt_event_get_subject(event), ==, "chief");
    g_assert_cmpstr(clawt_event_get_detail(event, "command"), ==,
                    "echo hello");
    g_assert_cmpint(clawt_event_get_detail_int(event, "exit"), ==, 0);

    /*
     * And nothing else. A command's output is the likeliest place in
     * this whole path for a secret to appear, and nothing may write one
     * into a log line -- so the absence is the assertion, not an
     * afterthought.
     */
    g_assert_null(clawt_event_get_detail(event, "stdout"));
    g_assert_null(clawt_event_get_detail(event, "stderr"));

    fixture_teardown(&fixture);
}

/*
 * And a command that could not run at all is recorded too.
 *
 * A refused or failing command is the one somebody looks up, so a trail
 * holding only the successes answers the wrong question. The exit is -1
 * rather than the entry being absent: "we do not know what it did" and
 * "it did not happen" are different facts, and only one of them is true
 * here.
 */
static void
test_a_refused_exec_is_audited_too(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtSandbox) sandbox = NULL;
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(JsonNode) response = NULL;
    ClawtAgent *agent;
    ClawtEvent *event;
    gboolean is_error = FALSE;

    fixture_setup(&fixture, "agents:\n  - id: chief\n");

    agent = clawt_agent_manager_get(fixture.agents, "chief");
    sandbox = clawt_sandbox_new(CLAWT_CONFINE_WORKSPACE, fixture.dir);
    computer = clawt_host_computer_new("chief", sandbox);
    clawt_computer_start(computer, NULL);
    clawt_agent_set_computer(agent, computer);

    /*
     * A working directory outside the confinement boundary, which the
     * sandbox refuses before anything is spawned -- so there is no exit
     * status anywhere, which is the case the -1 exists for.  An agent
     * trying to run something outside its boundary is also, of the
     * three, the entry somebody is most likely to go looking for.
     */
    response = call_tool(&fixture, "chief", "clawtilla_computer_exec",
                         "{\"command\":\"echo hello\","
                         "\"working_dir\":\"/etc\",\"timeout\":10}");
    response_text(response, &is_error);
    g_assert_true(is_error);

    event = recorded_exec(&fixture);
    g_assert_nonnull(event);
    g_assert_cmpstr(clawt_event_get_subject(event), ==, "chief");
    g_assert_cmpstr(clawt_event_get_detail(event, "command"), ==,
                    "echo hello");
    g_assert_cmpint(clawt_event_get_detail_int(event, "exit"), ==, -1);

    fixture_teardown(&fixture);
}

/* ── Growing the fleet ───────────────────────────────────────────── */

typedef struct {
    gchar      *created_id;
    gchar      *created_purpose;
    GHashTable *created_settings;
    gboolean    started;
    gboolean    refuse;
} FleetRecord;

static gchar *
fake_create_agent(const gchar  *agent_id,
                  const gchar  *purpose,
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
    g_free(record->created_purpose);
    record->created_purpose = g_strdup(purpose);
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
    g_clear_pointer(&record.created_purpose, g_free);
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
    /*
     * The purpose arrives as itself, not as a configuration key.
     *
     * It used to be inserted into the settings as `persona`, which is a
     * section in the schema rather than a value -- so it was written to
     * the config file and never read by anything, and the whole persona
     * an operator wrote was discarded without a word.
     */
    g_assert_cmpstr(record.created_purpose, ==, "You keep the notes.");
    g_assert_null(g_hash_table_lookup(record.created_settings, "persona"));
    g_assert_null(g_hash_table_lookup(record.created_settings, "purpose"));
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
    g_clear_pointer(&record.created_purpose, g_free);
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
    g_clear_pointer(&record.created_purpose, g_free);
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
    g_clear_pointer(&record.created_purpose, g_free);
    g_clear_pointer(&record.created_settings, g_hash_table_unref);
    fixture_teardown(&fixture);
}


/*
 * What TOOLS.org says an agent has must be what it has.
 *
 * The file used to carry a table written when the workspace was
 * scaffolded, so a tool granted afterwards never appeared in it -- and a
 * chief-of-staff asked whether it could create agents read its own file
 * and said no, on the day the tool was given to it. The description
 * comes from the same gate that answers tools/list, so the two cannot
 * disagree.
 */
static void
test_the_described_tools_are_the_permitted_ones(void)
{
    Fixture fixture = { 0 };
    FleetRecord record = { 0 };
    g_autofree gchar *before = NULL;
    g_autofree gchar *after = NULL;

    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: chief\n"
                  "    tools:\n"
                  "      manage_fleet: true\n"
                  "  - id: worker\n");

    clawt_mcp_tools_set_create_agent_func(fixture.tools, fake_create_agent,
                                          &record, NULL);

    before = clawt_mcp_tools_describe_for_agent(fixture.tools, "worker");
    after = clawt_mcp_tools_describe_for_agent(fixture.tools, "chief");

    g_assert_null(strstr(before, "clawtilla_create_agent"));
    g_assert_nonnull(strstr(after, "clawtilla_create_agent"));

    /* Both are told the list is current, not a snapshot. */
    g_assert_nonnull(strstr(after, "regenerated every time you start"));

    /*
     * And a tool that is missing is said to be missing, rather than the
     * agent being left to look for another way round.
     */
    g_assert_nonnull(strstr(before, "clawtilla_list_agents"));

    g_clear_pointer(&record.created_id, g_free);
    g_clear_pointer(&record.created_purpose, g_free);
    g_clear_pointer(&record.created_settings, g_hash_table_unref);
    fixture_teardown(&fixture);
}


/* ── Teams ───────────────────────────────────────────────────────── */

/*
 * A member is not offered the delegate tool at all.
 *
 * It could never succeed for them, and a tool that is listed and always
 * refused teaches an agent to keep trying it in different shapes.
 */
static void
test_a_member_is_not_offered_delegation(void)
{
    Fixture fixture = { 0 };

    fixture_setup(&fixture,
        "teams:\n  - id: research\n"
        "agents:\n"
        "  - id: boss\n    team: research\n    team_role: lead\n"
        "  - id: hand\n    team: research\n");

    g_assert_true(offers_tool(&fixture, "boss", "clawtilla_delegate"));
    g_assert_false(offers_tool(&fixture, "hand", "clawtilla_delegate"));

    /*
     * ...but everything about talking stays. The two roles differ over
     * assigning work, not over collaborating, and a member cut off from
     * its peers would be a much smaller thing than intended.
     */
    g_assert_true(offers_tool(&fixture, "hand", "clawtilla_message_agent"));
    g_assert_true(offers_tool(&fixture, "hand", "clawtilla_ask_agent"));
    g_assert_true(offers_tool(&fixture, "hand", "clawtilla_post_room"));
    g_assert_true(offers_tool(&fixture, "hand", "clawtilla_list_teams"));

    fixture_teardown(&fixture);
}

/*
 * A lead has the tool and is still refused outside its own team, because
 * which targets are allowed depends on the target rather than on the
 * caller -- so the tool list cannot answer it and the call must.
 */
static void
test_a_lead_is_refused_outside_its_team(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) response = NULL;
    const gchar *text;
    gboolean is_error = FALSE;

    fixture_setup(&fixture,
        "teams:\n  - id: research\n  - id: build\n"
        "agents:\n"
        "  - id: boss\n    team: research\n    team_role: lead\n"
        "  - id: builder\n    team: build\n");

    response = call_tool(&fixture, "boss", "clawtilla_delegate",
                         "{\"agent_id\":\"builder\",\"task\":\"x\"}");
    text = response_text(response, &is_error);

    g_assert_true(is_error);
    g_assert_nonnull(strstr(text, "not on your team"));
    g_assert_nonnull(strstr(text, "chief of staff"));

    fixture_teardown(&fixture);
}

/*
 * The teams listing leads with the description, because that is the part
 * a chief-of-staff actually matches work against -- names and members
 * say nothing about what a team handles.
 */
static void
test_the_team_listing_carries_the_descriptions(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) response = NULL;
    const gchar *text;
    gboolean is_error = TRUE;

    fixture_setup(&fixture,
        "teams:\n"
        "  - id: research\n    name: Research\n"
        "    description: Reads sources and answers questions.\n"
        "agents:\n"
        "  - id: chief\n    chief_of_staff: true\n"
        "  - id: boss\n    team: research\n    team_role: lead\n"
        "  - id: hand\n    team: research\n");

    response = call_tool(&fixture, "chief", "clawtilla_list_teams", "{}");
    text = response_text(response, &is_error);

    g_assert_false(is_error);
    g_assert_nonnull(strstr(text, "Reads sources"));
    g_assert_nonnull(strstr(text, "Lead: boss"));
    g_assert_nonnull(strstr(text, "hand"));
    g_assert_nonnull(strstr(text, "Running: 0 of 2"));

    fixture_teardown(&fixture);
}

/*
 * A fleet with no teams says so, rather than answering with nothing --
 * an agent that suspects there are teams it cannot see goes looking for
 * them.
 */
static void
test_a_fleet_with_no_teams_says_so(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) response = NULL;
    const gchar *text;
    gboolean is_error = TRUE;

    fixture_setup(&fixture,
                  "agents:\n  - id: chief\n    chief_of_staff: true\n");

    response = call_tool(&fixture, "chief", "clawtilla_list_teams", "{}");
    text = response_text(response, &is_error);

    g_assert_false(is_error);
    g_assert_nonnull(strstr(text, "no teams"));

    fixture_teardown(&fixture);
}

/*
 * A team with no description says that too. The chief cannot match work
 * against a blank, and the fix is a sentence from the user rather than a
 * guess from the agent.
 */
static void
test_a_team_with_no_description_says_so(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) response = NULL;
    const gchar *text;
    gboolean is_error = TRUE;

    fixture_setup(&fixture,
        "teams:\n  - id: research\n"
        "agents:\n  - id: chief\n    chief_of_staff: true\n");

    response = call_tool(&fixture, "chief", "clawtilla_list_teams", "{}");
    text = response_text(response, &is_error);

    g_assert_false(is_error);
    g_assert_nonnull(strstr(text, "No description"));

    fixture_teardown(&fixture);
}

/*
 * Work handed down a chain comes back up the same one.
 *
 * A chief of staff delegates to a lead, which delegates to a member, and
 * each of them reported straight into the operator's chat -- three
 * separate answers arriving in the middle of a conversation they were
 * not part of, and no answer to the thing that was actually asked. The
 * tool description used to encourage it in as many words.
 */
static void
test_message_user_is_refused_during_a_peers_turn(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) refused = NULL;
    g_autoptr(JsonNode) allowed = NULL;
    ClawtAgent *worker;
    const gchar *text;
    gboolean is_error = FALSE;

    fixture_setup(&fixture, "agents:\n  - id: chief\n  - id: worker\n");

    worker = clawt_agent_manager_get(fixture.agents, "worker");
    g_assert_nonnull(worker);

    /* The delivery that started this turn came from another agent. */
    clawt_agent_set_hop_depth(worker, 1);
    clawt_agent_set_turn_origin(worker, "chief");
    clawt_agent_begin_turn(worker, NULL);

    refused = call_tool(&fixture, "worker", "clawtilla_message_user",
                        "{\"body\": \"Done, boss.\"}");
    text = response_text(refused, &is_error);

    g_assert_true(is_error);

    /* It names who is waiting, and how to reach them. */
    g_assert_nonnull(strstr(text, "chief"));
    g_assert_nonnull(strstr(text, "clawtilla_message_agent"));

    /* And nothing was delivered anywhere. */
    g_assert_null(fixture.last_target);

    /*
     * A turn the operator started is untouched -- which is the case this
     * tool exists for, and refusing it would cut every agent off from
     * the person running the fleet.
     */
    clawt_agent_set_hop_depth(worker, 0);
    clawt_agent_set_turn_origin(worker, "user");
    clawt_agent_begin_turn(worker, NULL);

    is_error = TRUE;
    allowed = call_tool(&fixture, "worker", "clawtilla_message_user",
                        "{\"body\": \"Here is what you asked for.\"}");
    response_text(allowed, &is_error);

    g_assert_false(is_error);
    g_assert_nonnull(fixture.last_target);

    fixture_teardown(&fixture);
}

/*
 * And it is still refused on the peer's *second* message.
 *
 * The one above delivers once and begins one turn, which is the only
 * shape that ever passed through here -- and the guard held for it
 * throughout. What it cannot see is a peer that sends several messages
 * while the agent is busy: libreclaw queues them and runs a turn each,
 * and clawtilla armed the whole burst with a single "a delivery set the
 * next turn up" flag that the first turn spent. Turns two onwards read
 * as turns nothing delivered into, so the origin was gone and the guard
 * did not fire.
 *
 * On a real fleet that was three messages in the operator's chat from
 * one question: the answer, and two reports written for another agent
 * and pushed into a conversation they were not part of. Both tool calls
 * returned success, so nothing anywhere said the guard had been skipped.
 *
 * Three deliveries and three turns, because two cannot tell "the flag is
 * spent" from "the queue is one deep".
 */
static void
test_message_user_is_refused_on_a_peers_later_messages(void)
{
    Fixture fixture = { 0 };
    ClawtAgent *worker;
    guint turn;

    fixture_setup(&fixture, "agents:\n  - id: chief\n  - id: worker\n");

    worker = clawt_agent_manager_get(fixture.agents, "worker");
    g_assert_nonnull(worker);

    /*
     * Three messages from the same peer, all delivered before the agent
     * gets round to any of them.  That is what a busy agent looks like:
     * delivery acknowledges at the socket, so the mailbox empties while
     * the turns are still to come.
     */
    for (turn = 0; turn < 3; turn++)
        clawt_agent_deliver_turn(worker, NULL, 1, TRUE, "chief", NULL);

    for (turn = 0; turn < 3; turn++) {
        g_autoptr(JsonNode) refused = NULL;
        const gchar *text;
        gboolean is_error = FALSE;

        clawt_agent_begin_turn(worker, NULL);

        g_assert_cmpstr(clawt_agent_get_turn_origin(worker), ==, "chief");
        g_assert_cmpint(clawt_agent_get_hop_depth(worker), ==, 1);

        refused = call_tool(&fixture, "worker", "clawtilla_message_user",
                            "{\"body\": \"An update for you.\"}");
        text = response_text(refused, &is_error);

        g_assert_true(is_error);
        g_assert_nonnull(strstr(text, "chief"));

        /* And nothing reached the operator on any of the three. */
        g_assert_null(fixture.last_target);
    }

    /*
     * The fourth turn had no delivery behind it, so it is the operator's
     * and the tool works.  Without this the test passes against a build
     * that simply never clears the origin, which would cut every agent
     * off from the person running the fleet.
     */
    {
        g_autoptr(JsonNode) allowed = NULL;
        gboolean is_error = TRUE;

        clawt_agent_begin_turn(worker, NULL);
        g_assert_null(clawt_agent_get_turn_origin(worker));

        allowed = call_tool(&fixture, "worker", "clawtilla_message_user",
                            "{\"body\": \"Here is what you asked for.\"}");
        response_text(allowed, &is_error);

        g_assert_false(is_error);
        g_assert_nonnull(fixture.last_target);
    }

    fixture_teardown(&fixture);
}



/* ── Which conversation a tool call belongs to ───────────────────── */

/*
 * A tool call answered for the conversation it is actually part of.
 *
 * An agent runs a turn per room and can have several going, so the hop
 * depth, the turn origin and the task new work is parented on all depend
 * on which one.  The call itself arrives on a per-agent link with no room
 * on it, so libreclaw puts the session's room in the environment of the
 * CLI that spawns clawtilla-mcp-server and the server passes it here.
 *
 * Without it every answer is a fold across every turn the agent has
 * running -- safe, and wrong whenever it matters.
 */
static void
test_the_runtimes_room_picks_the_turn(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) shallow = NULL;
    g_autoptr(JsonNode) deep = NULL;
    ClawtAgent *lead;
    ClawtTask *one;
    ClawtTask *two;

    fixture_setup(&fixture,
        "agents:\n  - id: lead\n    chief_of_staff: true\n"
        "  - id: worker\n");

    lead = clawt_agent_manager_get(fixture.agents, "lead");

    one = clawt_task_manager_create(fixture.tasks, "user", "lead",
                                    "the first job", NULL, NULL);
    two = clawt_task_manager_create(fixture.tasks, "user", "lead",
                                    "the second job", NULL, NULL);

    /* Two conversations, at different depths, on different tasks. */
    clawt_agent_deliver_turn(lead, "room-a", 1, TRUE, "user",
                             clawt_task_get_id(one));
    clawt_agent_deliver_turn(lead, "room-b", 5, TRUE, "user",
                             clawt_task_get_id(two));
    clawt_agent_note_typing(lead, "room-a", TRUE);
    clawt_agent_note_typing(lead, "room-b", TRUE);

    /* Delegating from room-a parents on room-a's task, not room-b's. */
    shallow = call_tool_in(&fixture, "lead", "room-a", "clawtilla_delegate",
                           "{\"agent_id\":\"worker\",\"task\":\"a piece\"}");
    response_text(shallow, NULL);

    /* And from room-b, on room-b's. */
    deep = call_tool_in(&fixture, "lead", "room-b", "clawtilla_delegate",
                        "{\"agent_id\":\"worker\",\"task\":\"another\"}");
    response_text(deep, NULL);

    {
        g_autoptr(GPtrArray) children =
            clawt_task_manager_list(fixture.tasks, "worker", TRUE);
        gboolean saw_one = FALSE;
        gboolean saw_two = FALSE;
        guint i;

        g_assert_cmpuint(children->len, ==, 2);

        for (i = 0; i < children->len; i++) {
            const gchar *parent =
                clawt_task_get_parent_id(g_ptr_array_index(children, i));

            if (g_strcmp0(parent, clawt_task_get_id(one)) == 0)
                saw_one = TRUE;
            if (g_strcmp0(parent, clawt_task_get_id(two)) == 0)
                saw_two = TRUE;
        }

        /*
         * One under each.  Folded across both turns the tasks disagree,
         * so the answer would have been no parent at all -- twice.
         */
        g_assert_true(saw_one);
        g_assert_true(saw_two);
    }

    fixture_teardown(&fixture);
}

/*
 * An agent's own claim is believed only where it still has a turn.
 *
 * The runtime's answer is authoritative; this is the fallback for the
 * paths that do not carry it -- the agent repeating what its delivery
 * preamble told it.  A claim is worth what it can be checked against,
 * and here there is something to check: a turn's description outlives
 * the turn, deliberately, so that a reply posted after the indicator
 * drops can still be judged.  An agent naming a room whose turn has
 * *ended* would therefore be answered from it -- and naming an old,
 * shallow conversation buys a lower hop depth in the one it is really
 * in.
 */
static void
test_an_agents_claimed_room_is_checked(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) response = NULL;
    ClawtAgent *chief;

    fixture_setup(&fixture,
        "agents:\n  - id: chief\n    chief_of_staff: true\n"
        "  - id: worker\n");

    chief = clawt_agent_manager_get(fixture.agents, "chief");

    /* A shallow conversation that has finished. */
    clawt_agent_deliver_turn(chief, "room-shallow", 1, TRUE, "user", NULL);
    clawt_agent_note_typing(chief, "room-shallow", TRUE);
    clawt_agent_note_typing(chief, "room-shallow", FALSE);

    /* And a deep one that has not. */
    clawt_agent_deliver_turn(chief, "room-deep", 7, TRUE, "peer", NULL);
    clawt_agent_note_typing(chief, "room-deep", TRUE);

    response = call_tool_in(&fixture, "chief", NULL,
                            "clawtilla_message_agent",
                            "{\"agent_id\":\"worker\",\"body\":\"hello\","
                            "\"turn_room\":\"room-shallow\"}");
    response_text(response, NULL);

    /*
     * Answered from the turn it is actually in.  Believed, the finished
     * room would have stamped this message at depth 2 instead of 8 --
     * `orchestration.max_hops` measures the wrong conversation and a
     * chain that should have been cut carries on.
     */
    g_assert_cmpint(fixture.last_depth, ==, 8);

    fixture_teardown(&fixture);
}

/*
 * And the runtime wins when the two disagree.
 *
 * The environment is set by libreclaw per session; the argument is the
 * model repeating something back.  Where both are present there is no
 * reason to prefer the one that can be got wrong.
 */
static void
test_the_runtime_outranks_the_agents_claim(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) response = NULL;
    ClawtAgent *lead;
    ClawtTask *one;
    ClawtTask *two;

    fixture_setup(&fixture,
        "agents:\n  - id: lead\n    chief_of_staff: true\n"
        "  - id: worker\n");

    lead = clawt_agent_manager_get(fixture.agents, "lead");
    one = clawt_task_manager_create(fixture.tasks, "user", "lead", "first",
                                    NULL, NULL);
    two = clawt_task_manager_create(fixture.tasks, "user", "lead", "second",
                                    NULL, NULL);

    clawt_agent_deliver_turn(lead, "room-a", 1, TRUE, "user",
                             clawt_task_get_id(one));
    clawt_agent_deliver_turn(lead, "room-b", 1, TRUE, "user",
                             clawt_task_get_id(two));
    clawt_agent_note_typing(lead, "room-a", TRUE);
    clawt_agent_note_typing(lead, "room-b", TRUE);

    response = call_tool_in(&fixture, "lead", "room-a", "clawtilla_delegate",
                            "{\"agent_id\":\"worker\",\"task\":\"a piece\","
                            "\"turn_room\":\"room-b\"}");
    response_text(response, NULL);

    {
        g_autoptr(GPtrArray) children =
            clawt_task_manager_list(fixture.tasks, "worker", TRUE);

        g_assert_cmpuint(children->len, ==, 1);
        g_assert_cmpstr(
            clawt_task_get_parent_id(g_ptr_array_index(children, 0)),
            ==, clawt_task_get_id(one));
    }

    fixture_teardown(&fixture);
}

/* ── The task tree ───────────────────────────────────────────────── */

/*
 * Work an agent hands on belongs under the work it was given.
 *
 * clawtilla_delegate passed NULL as the parent for its whole life, so
 * every task an agent created was a root.  Two documented features were
 * inert because of it and neither reported anything: the depth limit
 * measured 0 however long the real chain was, and
 * clawtilla_task_cancel's promise of "and everything it spawned" found
 * no children to cascade to.
 */
static void
test_delegating_from_a_task_records_the_parent(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) response = NULL;
    g_autoptr(GPtrArray) children = NULL;
    ClawtAgent *lead;
    ClawtTask *root;
    gboolean is_error = TRUE;

    fixture_setup(&fixture,
        "agents:\n  - id: lead\n    chief_of_staff: true\n"
        "  - id: worker\n");

    root = clawt_task_manager_create(fixture.tasks, "user", "lead",
                                     "verify the guests", NULL, NULL);
    g_assert_nonnull(root);

    lead = clawt_agent_manager_get(fixture.agents, "lead");
    clawt_agent_deliver_turn(lead, NULL, 1, TRUE, "user",
                             clawt_task_get_id(root));
    clawt_agent_begin_turn(lead, NULL);

    response = call_tool(&fixture, "lead", "clawtilla_delegate",
                         "{\"agent_id\":\"worker\","
                         "\"task\":\"do the same on yours\"}");
    response_text(response, &is_error);
    g_assert_false(is_error);

    children = clawt_task_manager_list(fixture.tasks, "worker", TRUE);
    g_assert_cmpuint(children->len, ==, 1);

    {
        ClawtTask *child = g_ptr_array_index(children, 0);

        g_assert_cmpstr(clawt_task_get_parent_id(child), ==,
                        clawt_task_get_id(root));
        g_assert_cmpint(clawt_task_get_depth(child), ==, 1);
    }

    /* And the cascade the tool promises now reaches it. */
    g_assert_cmpuint(clawt_task_manager_cancel(fixture.tasks,
                                               clawt_task_get_id(root),
                                               "changed my mind"), ==, 2);

    fixture_teardown(&fixture);
}

/*
 * A turn nothing delegated into starts a root, as it always did.
 *
 * An operator typing, a routine, a webhook: none of them is a task, and
 * parenting their work onto whatever the agent last happened to handle
 * would be worse than no parent at all.
 */
static void
test_delegating_outside_a_task_starts_a_root(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) response = NULL;
    g_autoptr(GPtrArray) children = NULL;
    ClawtAgent *lead;
    gboolean is_error = TRUE;

    fixture_setup(&fixture,
        "agents:\n  - id: lead\n    chief_of_staff: true\n"
        "  - id: worker\n");

    lead = clawt_agent_manager_get(fixture.agents, "lead");
    clawt_agent_begin_turn(lead, NULL);

    response = call_tool(&fixture, "lead", "clawtilla_delegate",
                         "{\"agent_id\":\"worker\",\"task\":\"a fresh job\"}");
    response_text(response, &is_error);
    g_assert_false(is_error);

    children = clawt_task_manager_list(fixture.tasks, "worker", TRUE);
    g_assert_cmpuint(children->len, ==, 1);
    g_assert_null(clawt_task_get_parent_id(
                      g_ptr_array_index(children, 0)));

    fixture_teardown(&fixture);
}

/*
 * A turn state that has gone stale does not parent new work onto a
 * closed job.
 *
 * clawt_agent_begin_turn() only runs when the agent raises a typing
 * indicator, which needs a room -- so the id an agent is carrying is
 * only as fresh as its last one.  Checked against the manager rather
 * than trusted, because a finished task acquiring children is a tree
 * nobody can read.
 */
static void
test_a_finished_parent_is_not_used(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) response = NULL;
    g_autoptr(GPtrArray) children = NULL;
    ClawtAgent *lead;
    ClawtTask *root;
    gboolean is_error = TRUE;

    fixture_setup(&fixture,
        "agents:\n  - id: lead\n    chief_of_staff: true\n"
        "  - id: worker\n");

    root = clawt_task_manager_create(fixture.tasks, "user", "lead",
                                     "an old job", NULL, NULL);

    lead = clawt_agent_manager_get(fixture.agents, "lead");
    clawt_agent_deliver_turn(lead, NULL, 1, TRUE, "user",
                             clawt_task_get_id(root));
    clawt_agent_begin_turn(lead, NULL);

    g_assert_true(clawt_task_manager_complete(fixture.tasks,
                                              clawt_task_get_id(root),
                                              "done ages ago"));

    response = call_tool(&fixture, "lead", "clawtilla_delegate",
                         "{\"agent_id\":\"worker\",\"task\":\"something new\"}");
    response_text(response, &is_error);
    g_assert_false(is_error);

    children = clawt_task_manager_list(fixture.tasks, "worker", TRUE);
    g_assert_cmpuint(children->len, ==, 1);
    g_assert_null(clawt_task_get_parent_id(
                      g_ptr_array_index(children, 0)));

    fixture_teardown(&fixture);
}

/*
 * A delegator can list what its work turned into.
 *
 * clawtilla_task_list answered from the caller's own tasks alone, so a
 * chief could see the job it gave a lead and nothing the lead gave
 * anybody -- and the `agent_id` argument, documented as "tasks involving
 * this agent", actually filtered the caller's own tasks by counterparty.
 * Asking about a worker two levels down therefore reported "No tasks
 * involving kudu" while kudu's task was running, and blamed a daemon
 * restart for it.
 */
static void
test_task_list_shows_the_fan_out(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) response = NULL;
    g_autoptr(JsonNode) filtered = NULL;
    ClawtTask *root;
    ClawtTask *child;
    const gchar *text;

    fixture_setup(&fixture,
        "agents:\n  - id: chief\n    chief_of_staff: true\n"
        "  - id: oryx\n  - id: kudu\n");

    root = clawt_task_manager_create(fixture.tasks, "chief", "oryx",
                                     "verify the guests", NULL, NULL);
    child = clawt_task_manager_create(fixture.tasks, "oryx", "kudu",
                                      "yours too",
                                      clawt_task_get_id(root), NULL);

    response = call_tool(&fixture, "chief", "clawtilla_task_list", "{}");
    text = response_text(response, NULL);

    g_assert_nonnull(strstr(text, clawt_task_get_id(root)));
    g_assert_nonnull(strstr(text, "Handed on further"));
    g_assert_nonnull(strstr(text, clawt_task_get_id(child)));

    /* Both sides are named, since neither of them is the caller. */
    g_assert_nonnull(strstr(text, "oryx -> kudu"));

    /*
     * And asking about the agent two levels down finds it, which is the
     * question that used to come back empty.
     */
    filtered = call_tool(&fixture, "chief", "clawtilla_task_list",
                         "{\"agent_id\":\"kudu\"}");
    text = response_text(filtered, NULL);

    g_assert_nonnull(strstr(text, clawt_task_get_id(child)));

    fixture_teardown(&fixture);
}

/*
 * And when it really is empty, it says what it did and did not look at.
 *
 * "No tasks involving kudu" was false in two ways at once: the filter
 * meant "between you and kudu", and the sentence went on to blame a
 * daemon restart -- a plausible wrong cause for a task that was running
 * the whole time.
 */
static void
test_an_empty_filtered_listing_says_what_it_covered(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) response = NULL;
    const gchar *text;

    fixture_setup(&fixture,
        "agents:\n  - id: chief\n    chief_of_staff: true\n"
        "  - id: kudu\n");

    response = call_tool(&fixture, "chief", "clawtilla_task_list",
                         "{\"agent_id\":\"kudu\"}");
    text = response_text(response, NULL);

    g_assert_nonnull(strstr(text, "between you and kudu"));
    g_assert_nonnull(strstr(text, "somebody else"));

    fixture_teardown(&fixture);
}

/*
 * An assignee can end a turn without ending the task.
 *
 * The daemon completes a task from the message that ends its assignee's
 * turn, because an AI CLI cannot end one without writing something.
 * That punished the assignee doing the right thing: finish your share,
 * hand the rest on, report once at the end.  Such a turn ended with a
 * status note and the task closed under it -- with a result that said,
 * in so many words, that the report had not been sent yet.
 */
static void
test_reporting_progress_keeps_a_task_open(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) response = NULL;
    g_autoptr(JsonNode) status = NULL;
    g_autofree gchar *arguments = NULL;
    g_autofree gchar *held = NULL;
    ClawtTask *task;
    gboolean is_error = TRUE;
    const gchar *text;

    fixture_setup(&fixture, "agents:\n  - id: chief\n"
                            "    chief_of_staff: true\n  - id: oryx\n");

    task = clawt_task_manager_create(fixture.tasks, "chief", "oryx",
                                     "verify the guests", NULL, NULL);

    arguments = g_strdup_printf(
        "{\"task_id\":\"%s\",\"note\":\"mine is clean; waiting on the "
        "others\"}", clawt_task_get_id(task));

    response = call_tool(&fixture, "oryx", "clawtilla_task_progress",
                         arguments);
    text = response_text(response, &is_error);

    g_assert_false(is_error);

    /*
     * It says what it bought.  An agent that cannot tell whether the
     * call worked goes back to narrating in order to stay alive, which
     * is the behaviour this exists to make unnecessary.
     */
    g_assert_nonnull(strstr(text, "stays open"));

    /* Picked up, so nobody re-delegates it. */
    g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_RUNNING);

    /* And the turn ending no longer closes it. */
    g_assert_false(clawt_task_manager_complete_on_turn_end(
                       fixture.tasks, clawt_task_get_id(task),
                       "I will check back shortly", &held));
    g_assert_nonnull(held);
    g_assert_false(clawt_task_is_finished(task));

    /*
     * The delegator can read the note without waiting for a result --
     * and it is the note the assignee wrote for the purpose, not the
     * sentence its AI CLI happened to end the turn on.  Both are true
     * of the same moment and only one of them was chosen to be read.
     */
    {
        g_autofree gchar *id_arg = g_strdup_printf(
            "{\"task_id\":\"%s\"}", clawt_task_get_id(task));

        status = call_tool(&fixture, "chief", "clawtilla_task_status",
                           id_arg);
        text = response_text(status, NULL);

        g_assert_nonnull(strstr(text, "waiting on the others"));
        g_assert_null(strstr(text, "check back shortly"));
    }

    fixture_teardown(&fixture);
}

/*
 * Progress on a task that has already ended is refused rather than
 * silently accepted, so an assignee does not believe it has kept alive
 * something that was cancelled out from under it.
 */
static void
test_progress_on_a_finished_task_is_refused(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) response = NULL;
    g_autofree gchar *arguments = NULL;
    ClawtTask *task;
    gboolean is_error = FALSE;
    const gchar *text;

    fixture_setup(&fixture, "agents:\n  - id: chief\n"
                            "    chief_of_staff: true\n  - id: oryx\n");

    task = clawt_task_manager_create(fixture.tasks, "chief", "oryx",
                                     "verify the guests", NULL, NULL);
    clawt_task_manager_cancel(fixture.tasks, clawt_task_get_id(task),
                              "no longer needed");

    arguments = g_strdup_printf("{\"task_id\":\"%s\",\"note\":\"still on it\"}",
                                clawt_task_get_id(task));

    response = call_tool(&fixture, "oryx", "clawtilla_task_progress",
                         arguments);
    text = response_text(response, &is_error);

    g_assert_true(is_error);
    g_assert_nonnull(strstr(text, "already"));

    fixture_teardown(&fixture);
}

/*
 * A result nobody reported says so when it is read.
 *
 * "They said it was done" and "they stopped talking and this is the last
 * thing they wrote" are different facts, and a delegator that cannot
 * tell them apart either waits on finished work or redoes it.
 */
static void
test_an_inferred_result_is_labelled(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) response = NULL;
    g_autofree gchar *arguments = NULL;
    ClawtTask *task;
    const gchar *text;

    fixture_setup(&fixture, "agents:\n  - id: chief\n"
                            "    chief_of_staff: true\n  - id: oryx\n");

    task = clawt_task_manager_create(fixture.tasks, "chief", "oryx",
                                     "verify the guests", NULL, NULL);
    arguments = g_strdup_printf("{\"task_id\":\"%s\"}",
                                clawt_task_get_id(task));

    g_assert_true(clawt_task_manager_complete_on_turn_end(
                      fixture.tasks, clawt_task_get_id(task),
                      "looks fine to me", NULL));

    response = call_tool(&fixture, "chief", "clawtilla_task_result",
                         arguments);
    text = response_text(response, NULL);

    g_assert_nonnull(strstr(text, "looks fine to me"));
    g_assert_nonnull(strstr(text, "Nobody reported this task complete"));

    fixture_teardown(&fixture);
}

/* ── Commands that need a shell ──────────────────────────────────── */

/*
 * A command line that needs a shell is refused, and never half-run.
 *
 * g_shell_parse_argv() lexes without any shell semantics, so `;`, `&&`,
 * `|`, redirections and `$(...)` all reached argv as literal text and
 * the program ran anyway: exit 0, the rest of the line echoed back, and
 * nothing logged.  From the agent's side that reads as a command that
 * ran and behaved oddly, which is the most expensive kind of wrong.
 */
static void
test_a_command_needing_a_shell_is_refused(void)
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
                         "{\"command\":\"echo one; echo two\","
                         "\"timeout\":10}");
    text = response_text(response, &is_error);

    g_assert_true(is_error);
    g_assert_nonnull(strstr(text, "bash -c"));

    /*
     * And nothing ran.  The refusal is before the parse, so there is no
     * audit entry either -- a command that was refused did not happen,
     * and a trail saying otherwise answers the wrong question.
     */
    g_assert_nonnull(strstr(text, ";"));
    g_assert_null(recorded_exec(&fixture));

    fixture_teardown(&fixture);
}

/*
 * The route it recommends works, so the refusal has something to
 * recommend.  bash -c is inspected the same way anything else is --
 * clawt_sandbox_check_argv() re-parses the nested command line -- so
 * this is a supported route rather than a way around confinement.
 */
static void
test_the_shell_it_recommends_actually_runs(void)
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
    sandbox = clawt_sandbox_new(CLAWT_CONFINE_NONE, fixture.dir);
    computer = clawt_host_computer_new("chief", sandbox);
    clawt_computer_start(computer, NULL);
    clawt_agent_set_computer(agent, computer);

    response = call_tool(&fixture, "chief", "clawtilla_computer_exec",
                         "{\"command\":\"bash -c 'echo one; echo two'\","
                         "\"timeout\":10}");
    text = response_text(response, &is_error);

    g_assert_false(is_error);
    g_assert_nonnull(strstr(text, "one"));
    g_assert_nonnull(strstr(text, "two"));

    fixture_teardown(&fixture);
}

/*
 * And the tool says so before anybody tries it.
 *
 * A tool's description is part of its behaviour.  An agent reads the
 * description once and writes shell lines for the rest of its life, so
 * a refusal that only arrives at call time costs a turn every time.
 */
static void
test_the_exec_tool_says_it_is_not_a_shell(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtSandbox) sandbox = NULL;
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(JsonNode) listing = NULL;
    ClawtAgent *agent;
    JsonArray *tools;
    gboolean checked = FALSE;
    guint i;

    fixture_setup(&fixture, "agents:\n  - id: chief\n");

    agent = clawt_agent_manager_get(fixture.agents, "chief");
    sandbox = clawt_sandbox_new(CLAWT_CONFINE_NONE, fixture.dir);
    computer = clawt_host_computer_new("chief", sandbox);
    clawt_computer_start(computer, NULL);
    clawt_agent_set_computer(agent, computer);

    listing = clawt_mcp_tools_list(fixture.tools, "chief");
    tools = json_object_get_array_member(json_node_get_object(listing),
                                         "tools");

    for (i = 0; i < json_array_get_length(tools); i++) {
        JsonObject *tool = json_array_get_object_element(tools, i);

        if (g_strcmp0(json_object_get_string_member(tool, "name"),
                      "clawtilla_computer_exec") != 0)
            continue;

        g_assert_nonnull(strstr(json_object_get_string_member(tool,
                                                              "description"),
                                "not a shell"));
        g_assert_nonnull(strstr(json_object_get_string_member(tool,
                                                              "description"),
                                "bash -c"));
        checked = TRUE;
    }

    g_assert_true(checked);

    fixture_teardown(&fixture);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/mcp/team/member-cannot-delegate",
                    test_a_member_is_not_offered_delegation);
    g_test_add_func("/mcp/team/lead-stops-at-its-team",
                    test_a_lead_is_refused_outside_its_team);
    g_test_add_func("/mcp/team/listing-carries-descriptions",
                    test_the_team_listing_carries_the_descriptions);
    g_test_add_func("/mcp/team/no-teams-says-so",
                    test_a_fleet_with_no_teams_says_so);
    g_test_add_func("/mcp/team/no-description-says-so",
                    test_a_team_with_no_description_says_so);
    g_test_add_func("/mcp/fleet/description-matches-the-gate",
                    test_the_described_tools_are_the_permitted_ones);
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
    g_test_add_func("/mcp/message-carries-the-priority",
                    test_message_agent_carries_the_priority);
    g_test_add_func("/mcp/message-with-no-band-is-normal",
                    test_a_message_with_no_band_is_normal);
    g_test_add_func("/mcp/unknown-band-is-refused",
                    test_an_unknown_band_is_refused_before_anything_is_sent);
    g_test_add_func("/mcp/ask-agent-queues",
                    test_ask_agent_queues_rather_than_waiting);
    g_test_add_func("/mcp/missing-arguments", test_missing_arguments_are_named);
    g_test_add_func("/mcp/delegate", test_delegate_creates_a_task);
    g_test_add_func("/mcp/delegate-unknown",
                    test_delegate_to_unknown_agent_suggests_listing);
    g_test_add_func("/mcp/undeliverable-delegation",
                    test_undeliverable_delegation_fails_its_task);

    g_test_add_func("/mcp/task-status-result", test_task_status_and_result);
    g_test_add_func("/mcp/task-list/both-directions",
                    test_task_list_shows_both_directions);
    g_test_add_func("/mcp/task-list/explains-pending",
                    test_task_list_explains_pending);
    g_test_add_func("/mcp/task-list/empty-says-why",
                    test_an_empty_task_list_says_why);
    g_test_add_func("/mcp/task-list/scoped-to-caller",
                    test_task_list_is_scoped_to_the_caller);
    g_test_add_func("/mcp/assignee-completes",
                    test_assignee_can_complete_its_task);

    g_test_add_func("/mcp/mailbox-list", test_mailbox_list_reports_waiting_messages);
    g_test_add_func("/mcp/room-history-by-agent",
                    test_room_history_takes_an_agent_id);

    g_test_add_func("/mcp/computer-exec", test_computer_exec_through_the_tool);
    g_test_add_func("/mcp/async-exec-answers-on-the-named-context",
                    test_an_async_exec_answers_on_the_named_context);
    g_test_add_func("/mcp/audit/an-agents-exec-is-recorded",
                    test_an_agents_exec_is_audited);
    g_test_add_func("/mcp/audit/a-refused-exec-is-recorded",
                    test_a_refused_exec_is_audited_too);
    g_test_add_func("/mcp/message-user/refused-during-a-peers-turn",
                    test_message_user_is_refused_during_a_peers_turn);
    g_test_add_func("/mcp/message-user/refused-on-later-messages",
                    test_message_user_is_refused_on_a_peers_later_messages);
    g_test_add_func("/mcp/failing-command", test_failing_command_reports_why);

    g_test_add_func("/mcp/turn-room/the-runtimes-room-picks-the-turn",
                    test_the_runtimes_room_picks_the_turn);
    g_test_add_func("/mcp/turn-room/an-agents-claim-is-checked",
                    test_an_agents_claimed_room_is_checked);
    g_test_add_func("/mcp/turn-room/the-runtime-outranks-the-claim",
                    test_the_runtime_outranks_the_agents_claim);
    g_test_add_func("/mcp/delegate/records-the-parent-task",
                    test_delegating_from_a_task_records_the_parent);
    g_test_add_func("/mcp/delegate/outside-a-task-starts-a-root",
                    test_delegating_outside_a_task_starts_a_root);
    g_test_add_func("/mcp/delegate/a-finished-parent-is-not-used",
                    test_a_finished_parent_is_not_used);
    g_test_add_func("/mcp/task-list/shows-the-fan-out",
                    test_task_list_shows_the_fan_out);
    g_test_add_func("/mcp/task-list/empty-says-what-it-covered",
                    test_an_empty_filtered_listing_says_what_it_covered);
    g_test_add_func("/mcp/task-progress/keeps-a-task-open",
                    test_reporting_progress_keeps_a_task_open);
    g_test_add_func("/mcp/task-progress/refused-once-finished",
                    test_progress_on_a_finished_task_is_refused);
    g_test_add_func("/mcp/task-result/an-inferred-result-is-labelled",
                    test_an_inferred_result_is_labelled);
    g_test_add_func("/mcp/computer-exec/a-shell-line-is-refused",
                    test_a_command_needing_a_shell_is_refused);
    g_test_add_func("/mcp/computer-exec/the-recommended-shell-runs",
                    test_the_shell_it_recommends_actually_runs);
    g_test_add_func("/mcp/computer-exec/the-tool-says-it-is-not-a-shell",
                    test_the_exec_tool_says_it_is_not_a_shell);

    return g_test_run();
}
