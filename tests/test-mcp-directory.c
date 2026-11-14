/*
 * test-mcp-directory.c - A server with too many tools, as three tools
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The directory sits in the relay, which is the only place a connector's
 * `tools:` grant is enforced.  So the tests that matter most here are
 * negative ones: a tool this agent may not use is not found by the
 * search, not described, and not run by the call -- and an approval asked
 * about a call through the directory names the tool that would run.
 * Everything is driven through the same policy functions the stdio relay
 * and the HTTP bridge call, with no server process at all.
 */

#include <clawtilla.h>

#include <string.h>

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#include "clawt-test-util.h"

/* ── Fixtures ────────────────────────────────────────────────────── */

static JsonObject *
parse(const gchar *text)
{
	g_autoptr(JsonParser) parser = json_parser_new();

	g_assert_nonnull(text);
	g_assert_true(json_parser_load_from_data(parser, text, -1, NULL));

	return json_object_ref(json_node_get_object(
		json_parser_get_root(parser)));
}

/* One tool definition, with a schema of one required string field. */
static JsonObject *
make_tool(const gchar *name, const gchar *description, const gchar *field)
{
	JsonObject *tool = json_object_new();
	JsonObject *schema = json_object_new();
	JsonObject *props = json_object_new();
	JsonObject *prop = json_object_new();
	JsonArray *required = json_array_new();

	json_object_set_string_member(tool, "name", name);
	json_object_set_string_member(tool, "description", description);
	json_object_set_string_member(prop, "type", "string");
	json_object_set_object_member(props, field, prop);
	json_object_set_string_member(schema, "type", "object");
	json_object_set_object_member(schema, "properties", props);
	json_array_add_string_element(required, field);
	json_object_set_array_member(schema, "required", required);
	json_object_set_object_member(tool, "inputSchema", schema);

	return tool;
}

/* A forge-like server: issues, pulls, and one tool nobody should get. */
static JsonArray *
forge_tools(void)
{
	JsonArray *tools = json_array_new();

	json_array_add_object_element(tools,
		make_tool("issues_create", "Create a new issue in a repository.",
			  "title"));
	json_array_add_object_element(tools,
		make_tool("issues_list", "List the issues of a repository.",
			  "repo"));
	json_array_add_object_element(tools,
		make_tool("issues_comment", "Add a comment to an issue.",
			  "body"));
	json_array_add_object_element(tools,
		make_tool("pulls_merge", "Merge a pull request into its base "
			  "branch.", "number"));
	json_array_add_object_element(tools,
		make_tool("pulls_list", "List open pull requests.", "repo"));
	json_array_add_object_element(tools,
		make_tool("repos_delete", "Delete a repository for ever.",
			  "repo"));

	return tools;
}

/* The forge, minus repos_delete: what a narrowed connector permits. */
static GStrv
forge_permitted(void)
{
	const gchar *tools[] = { "issues_create", "issues_list",
				 "issues_comment", "pulls_merge",
				 "pulls_list", NULL };

	return g_strdupv((GStrv)tools);
}

/* A `tools/list` reply carrying @tools, and @cursor if there is a next page. */
static gchar *
list_reply(const gchar *id_json, JsonArray *tools, const gchar *cursor)
{
	g_autoptr(JsonGenerator) generator = json_generator_new();
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_ARRAY);
	g_autofree gchar *array = NULL;

	json_node_set_array(node, tools);
	json_generator_set_root(generator, node);
	array = json_generator_to_data(generator, NULL);

	if (cursor != NULL)
		return g_strdup_printf("{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":"
				       "{\"tools\":%s,\"nextCursor\":\"%s\"}}",
				       id_json, array, cursor);

	return g_strdup_printf("{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":"
			       "{\"tools\":%s}}", id_json, array);
}

static gchar *
call_line(gint id, const gchar *tool, const gchar *arguments_json)
{
	return g_strdup_printf("{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":"
			       "\"tools/call\",\"params\":{\"name\":\"%s\","
			       "\"arguments\":%s}}", id, tool, arguments_json);
}

/* The text of a tool result the directory answered with. */
static gchar *
result_text(const gchar *reply, gboolean *is_error)
{
	g_autoptr(JsonObject) object = parse(reply);
	JsonObject *result = json_object_get_object_member(object, "result");
	JsonArray *content = json_object_get_array_member(result, "content");

	if (is_error != NULL)
		*is_error = json_object_get_boolean_member_with_default(
			result, "isError", FALSE);

	return g_strdup(json_object_get_string_member(
		json_array_get_object_element(content, 0), "text"));
}

/* The names in a `tools/list` reply, comma joined, for one assertion. */
static gchar *
listed_names(const gchar *reply)
{
	g_autoptr(JsonObject) object = parse(reply);
	JsonObject *result = json_object_get_object_member(object, "result");
	JsonArray *tools = json_object_get_array_member(result, "tools");
	GString *out = g_string_new(NULL);
	guint i;

	for (i = 0; i < json_array_get_length(tools); i++) {
		if (i > 0)
			g_string_append_c(out, ',');

		g_string_append(out, json_object_get_string_member(
			json_array_get_object_element(tools, i), "name"));
	}

	return g_string_free(out, FALSE);
}

/*
 * Runs one `tools/list` round trip through @policy and returns what the
 * client was sent.
 */
static gchar *
list_through(ClawtMcpRelayPolicy *policy, JsonArray *tools)
{
	g_autofree gchar *to_server = NULL;
	g_autofree gchar *to_client = NULL;
	g_autofree gchar *reply = NULL;
	gchar *answer = NULL;
	g_autofree gchar *again = NULL;

	clawt_mcp_relay_policy_outbound(
		policy, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}",
		&to_server, &to_client);
	g_assert_nonnull(to_server);
	g_assert_null(to_client);

	reply = list_reply("1", tools, NULL);
	clawt_mcp_relay_policy_inbound(policy, reply, &answer, &again);
	g_assert_null(again);
	g_assert_nonnull(answer);

	return answer;
}

/* ── The thresholds ──────────────────────────────────────────────── */

static void
test_big_by_count(void)
{
	g_assert_false(clawt_mcp_directory_is_big(40, 0, 40, 100000));
	g_assert_true(clawt_mcp_directory_is_big(41, 0, 40, 100000));

	/* And through a real directory, with the default threshold. */
	{
		g_autoptr(ClawtMcpDirectory) directory = clawt_mcp_directory_new(
			"forge", CLAWT_TOOL_DIRECTORY_AUTO, FALSE, 40, 100000);
		g_autoptr(JsonArray) tools = json_array_new();
		guint i;

		for (i = 0; i < 41; i++) {
			g_autofree gchar *name = g_strdup_printf("t_%u", i);

			json_array_add_object_element(tools,
				make_tool(name, "A tool.", "x"));
		}

		g_assert_true(clawt_mcp_directory_load(directory, tools));
		g_assert_cmpuint(clawt_mcp_directory_get_tool_count(directory),
				 ==, 41);

		json_array_remove_element(tools, 0);
		g_assert_false(clawt_mcp_directory_load(directory, tools));
	}
}

static void
test_big_by_chars(void)
{
	g_autoptr(ClawtMcpDirectory) directory = clawt_mcp_directory_new(
		"docs", CLAWT_TOOL_DIRECTORY_AUTO, FALSE, 40, 100000);
	g_autoptr(JsonArray) tools = json_array_new();
	g_autofree gchar *essay = g_strnfill(30000, 'w');
	guint i;

	g_assert_false(clawt_mcp_directory_is_big(3, 100000, 40, 100000));
	g_assert_true(clawt_mcp_directory_is_big(3, 100001, 40, 100000));

	/* Four tools, each with a page of documentation: few, but heavy. */
	for (i = 0; i < 4; i++) {
		g_autofree gchar *name = g_strdup_printf("doc_%u", i);

		json_array_add_object_element(tools,
			make_tool(name, essay, "x"));
	}

	g_assert_cmpuint(clawt_mcp_directory_measure(tools), >, 120000);
	g_assert_true(clawt_mcp_directory_load(directory, tools));

	json_array_remove_element(tools, 0);
	json_array_remove_element(tools, 0);
	g_assert_false(clawt_mcp_directory_load(directory, tools));
}

/* ── The modes ───────────────────────────────────────────────────── */

static void
test_override_modes(void)
{
	ClawtToolDirectoryMode mode;
	guint i;

	/* always: even three tools, even for a CLI that searches. */
	g_assert_true(clawt_mcp_directory_decide(CLAWT_TOOL_DIRECTORY_ALWAYS,
						 TRUE, 3, 10, 40, 100000));

	/* never: even four hundred. */
	g_assert_false(clawt_mcp_directory_decide(CLAWT_TOOL_DIRECTORY_NEVER,
						  FALSE, 400, 1000000, 40,
						  100000));

	/* auto: big, but the CLI searches by itself. */
	g_assert_false(clawt_mcp_directory_decide(CLAWT_TOOL_DIRECTORY_AUTO,
						  TRUE, 400, 1000000, 40,
						  100000));
	g_assert_true(clawt_mcp_directory_decide(CLAWT_TOOL_DIRECTORY_AUTO,
						 FALSE, 400, 1000000, 40,
						 100000));

	/* Who searches: Claude Code, and nobody else yet. */
	g_assert_true(clawt_mcp_directory_cli_searches_tools("claude-code"));
	g_assert_true(clawt_mcp_directory_cli_searches_tools("claude-tmux"));
	g_assert_false(clawt_mcp_directory_cli_searches_tools("opencode"));
	g_assert_false(clawt_mcp_directory_cli_searches_tools("grok-build"));

	/* A spelling no mode has is reported, and read as auto, not as zero. */
	g_assert_false(clawt_tool_directory_mode_from_nick("sometimes", &mode));
	g_assert_cmpint(mode, ==, CLAWT_TOOL_DIRECTORY_AUTO);
	g_assert_true(clawt_tool_directory_mode_from_nick(NULL, &mode));
	g_assert_cmpint(mode, ==, CLAWT_TOOL_DIRECTORY_AUTO);

	/* The enumeration the clients walk agrees with the enum's nicks. */
	g_assert_cmpuint(clawt_tool_directory_mode_count(), ==, 3);

	for (i = 0; i < clawt_tool_directory_mode_count(); i++) {
		g_assert_true(clawt_tool_directory_mode_from_nick(
			clawt_tool_directory_mode_nth_nick(i), &mode));
		g_assert_cmpint(mode, ==, clawt_tool_directory_mode_nth(i));
		g_assert_nonnull(clawt_tool_directory_mode_nth_label(i));
	}

	/* never still counts, and touches nothing. */
	{
		g_autoptr(ClawtMcpDirectory) directory = clawt_mcp_directory_new(
			"forge", CLAWT_TOOL_DIRECTORY_NEVER, FALSE, 1, 1);
		ClawtMcpRelayPolicy policy = { 0 };
		g_autoptr(JsonArray) tools = forge_tools();
		g_autofree gchar *answer = NULL;
		g_autofree gchar *names = NULL;

		policy.directory = directory;
		answer = list_through(&policy, tools);
		names = listed_names(answer);

		g_assert_cmpstr(names, ==, "issues_create,issues_list,"
				"issues_comment,pulls_merge,pulls_list,"
				"repos_delete");
		g_assert_false(clawt_mcp_directory_is_active(directory));
		g_assert_cmpuint(clawt_mcp_directory_get_tool_count(directory),
				 ==, 6);
	}
}

/* ── Search ──────────────────────────────────────────────────────── */

static gchar *
first_match(ClawtMcpDirectory *directory, const gchar *query)
{
	g_autofree gchar *text = clawt_mcp_directory_search(directory, query, 0);
	g_autoptr(JsonObject) answer = parse(text);
	JsonArray *matches = json_object_get_array_member(answer, "matches");

	if (json_array_get_length(matches) == 0)
		return NULL;

	return g_strdup(json_object_get_string_member(
		json_array_get_object_element(matches, 0), "name"));
}

static void
test_search_ranking(void)
{
	g_autoptr(ClawtMcpDirectory) directory = clawt_mcp_directory_new(
		"forge", CLAWT_TOOL_DIRECTORY_ALWAYS, FALSE, 40, 100000);
	g_autoptr(JsonArray) tools = forge_tools();
	g_autofree gchar *create = NULL;
	g_autofree gchar *merge = NULL;
	g_autofree gchar *exact = NULL;
	g_autofree gchar *plural = NULL;
	g_autofree gchar *nothing = NULL;
	g_autofree gchar *overview = NULL;
	g_autofree gchar *area = NULL;

	clawt_mcp_directory_load(directory, tools);

	create = first_match(directory, "create an issue");
	merge = first_match(directory, "merge a pull request");
	exact = first_match(directory, "pulls_list");
	plural = first_match(directory, "comments");

	g_assert_cmpstr(create, ==, "issues_create");
	g_assert_cmpstr(merge, ==, "pulls_merge");
	g_assert_cmpstr(exact, ==, "pulls_list");
	g_assert_cmpstr(plural, ==, "issues_comment");

	/* Nothing matched is said, with the areas to try instead. */
	nothing = clawt_mcp_directory_search(directory, "xylophone", 0);
	g_assert_nonnull(strstr(nothing, "Nothing matched"));
	g_assert_nonnull(strstr(nothing, "issues (3)"));

	/* An empty query is the area-by-area overview. */
	overview = clawt_mcp_directory_search(directory, "  ", 0);
	g_assert_nonnull(strstr(overview, "\"areas\""));
	g_assert_nonnull(strstr(overview, "\"area\":\"issues\""));
	g_assert_nonnull(strstr(overview, "\"area\":\"pulls\""));

	area = clawt_mcp_directory_tool_area("createIssue");
	g_assert_cmpstr(area, ==, "create");
}

/* ── Permitted only ──────────────────────────────────────────────── */

static void
test_permitted_only(void)
{
	g_autoptr(ClawtMcpDirectory) directory = clawt_mcp_directory_new(
		"forge", CLAWT_TOOL_DIRECTORY_ALWAYS, FALSE, 40, 100000);
	g_auto(GStrv) permitted = forge_permitted();
	g_autoptr(JsonArray) tools = forge_tools();
	ClawtMcpRelayPolicy policy = { 0 };
	g_autofree gchar *answer = NULL;
	g_autofree gchar *names = NULL;

	policy.permitted = permitted;
	policy.filter = TRUE;
	policy.hint = "Narrowed.";
	policy.directory = directory;

	answer = list_through(&policy, tools);
	names = listed_names(answer);

	/* The three tools stand in for the five permitted ones. */
	g_assert_cmpstr(names, ==, "forge_search_tools,forge_describe_tool,"
			"forge_call_tool");
	g_assert_true(clawt_mcp_directory_is_active(directory));
	g_assert_cmpuint(clawt_mcp_directory_get_tool_count(directory), ==, 5);
	g_assert_false(clawt_mcp_directory_has_tool(directory, "repos_delete"));

	/* Not found... */
	{
		g_autofree gchar *line = call_line(2, "forge_search_tools",
						   "{\"query\":\"delete a "
						   "repository\"}");
		g_autofree gchar *to_server = NULL;
		g_autofree gchar *to_client = NULL;
		g_autofree gchar *text = NULL;

		clawt_mcp_relay_policy_outbound(&policy, line, &to_server,
						&to_client);
		g_assert_null(to_server);
		text = result_text(to_client, NULL);
		g_assert_null(strstr(text, "repos_delete"));
	}

	/* ...not described... */
	{
		g_autofree gchar *line = call_line(3, "forge_describe_tool",
						   "{\"name\":\"repos_delete\"}");
		g_autofree gchar *to_server = NULL;
		g_autofree gchar *to_client = NULL;
		g_autofree gchar *text = NULL;
		gboolean is_error = FALSE;

		clawt_mcp_relay_policy_outbound(&policy, line, &to_server,
						&to_client);
		g_assert_null(to_server);
		text = result_text(to_client, &is_error);
		g_assert_true(is_error);
		g_assert_nonnull(strstr(text, "No tool named"));
		g_assert_null(strstr(text, "Delete a repository"));
	}

	/* ...and not called: nothing reaches the server. */
	{
		g_autofree gchar *line = call_line(4, "forge_call_tool",
						   "{\"name\":\"repos_delete\","
						   "\"arguments\":{\"repo\":\"x\"}}");
		g_autofree gchar *to_server = NULL;
		g_autofree gchar *to_client = NULL;
		gboolean is_error = FALSE;
		g_autofree gchar *text = NULL;

		clawt_mcp_relay_policy_outbound(&policy, line, &to_server,
						&to_client);
		g_assert_null(to_server);
		text = result_text(to_client, &is_error);
		g_assert_true(is_error);
	}

	/* Called by its own name, around the directory: the filter refuses. */
	{
		g_autofree gchar *line = call_line(5, "repos_delete",
						   "{\"repo\":\"x\"}");
		g_autofree gchar *to_server = NULL;
		g_autofree gchar *to_client = NULL;

		clawt_mcp_relay_policy_outbound(&policy, line, &to_server,
						&to_client);
		g_assert_null(to_server);
		g_assert_nonnull(strstr(to_client, "does not permit"));
	}

	/* A permitted tool is described, and called as itself. */
	{
		g_autofree gchar *describe = clawt_mcp_directory_describe(
			directory, "issues_create");
		g_autofree gchar *line = call_line(6, "forge_call_tool",
						   "{\"name\":\"issues_create\","
						   "\"arguments\":{\"title\":"
						   "\"Broken\"}}");
		g_autofree gchar *to_server = NULL;
		g_autofree gchar *to_client = NULL;
		g_autoptr(JsonObject) sent = NULL;
		JsonObject *params;

		g_assert_nonnull(describe);
		g_assert_nonnull(strstr(describe, "\"inputSchema\""));

		clawt_mcp_relay_policy_outbound(&policy, line, &to_server,
						&to_client);
		g_assert_null(to_client);
		g_assert_nonnull(to_server);

		sent = parse(to_server);
		params = json_object_get_object_member(sent, "params");
		g_assert_cmpint(json_object_get_int_member(sent, "id"), ==, 6);
		g_assert_cmpstr(json_object_get_string_member(params, "name"),
				==, "issues_create");
		g_assert_cmpstr(json_object_get_string_member(
			json_object_get_object_member(params, "arguments"),
			"title"), ==, "Broken");
	}

	/* A batch would carry calls past all of it, so it is refused. */
	{
		g_autofree gchar *to_server = NULL;
		g_autofree gchar *to_client = NULL;
		ClawtMcpRelayPolicy open = { 0 };

		open.directory = directory;
		clawt_mcp_relay_policy_outbound(
			&open, "[{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":"
			"\"tools/call\",\"params\":{\"name\":\"repos_delete\"}}]",
			&to_server, &to_client);
		g_assert_null(to_server);
		g_assert_nonnull(strstr(to_client, "batched"));
	}
}

/* ── Approvals ───────────────────────────────────────────────────── */

typedef struct {
	GPtrArray *asked;
	gboolean   allow;
} GateLog;

static gboolean
recording_gate(const gchar *tool, gchar **refusal, gpointer user_data)
{
	GateLog *log = user_data;

	g_ptr_array_add(log->asked, g_strdup(tool));

	if (!log->allow)
		*refusal = g_strdup("Refused by a rule.");

	return log->allow;
}

static void
test_approval_names_inner_tool(void)
{
	g_autoptr(ClawtMcpDirectory) directory = clawt_mcp_directory_new(
		"forge", CLAWT_TOOL_DIRECTORY_ALWAYS, FALSE, 40, 100000);
	g_autoptr(JsonArray) tools = forge_tools();
	g_autoptr(GPtrArray) asked = g_ptr_array_new_with_free_func(g_free);
	GateLog log = { NULL, TRUE };
	ClawtMcpRelayPolicy policy = { 0 };
	g_autofree gchar *answer = NULL;
	guint i;

	log.asked = asked;
	policy.gate = recording_gate;
	policy.gate_data = &log;
	policy.directory = directory;

	answer = list_through(&policy, tools);

	/* A search runs nothing, so nobody is asked about it. */
	{
		g_autofree gchar *line = call_line(2, "forge_search_tools",
						   "{\"query\":\"merge\"}");
		g_autofree gchar *to_server = NULL;
		g_autofree gchar *to_client = NULL;

		clawt_mcp_relay_policy_outbound(&policy, line, &to_server,
						&to_client);
		g_assert_cmpuint(asked->len, ==, 0);
	}

	/* A call through the directory is asked about by the inner name. */
	{
		g_autofree gchar *line = call_line(3, "forge_call_tool",
						   "{\"name\":\"pulls_merge\","
						   "\"arguments\":{\"number\":"
						   "\"7\"}}");
		g_autofree gchar *to_server = NULL;
		g_autofree gchar *to_client = NULL;

		clawt_mcp_relay_policy_outbound(&policy, line, &to_server,
						&to_client);
		g_assert_nonnull(to_server);
		g_assert_cmpuint(asked->len, ==, 1);
		g_assert_cmpstr(g_ptr_array_index(asked, 0), ==, "pulls_merge");
	}

	/* And a refusal stops it, answered under the caller's own id. */
	{
		g_autofree gchar *line = call_line(4, "forge_call_tool",
						   "{\"name\":\"issues_create\","
						   "\"arguments\":{}}");
		g_autofree gchar *to_server = NULL;
		g_autofree gchar *to_client = NULL;
		g_autoptr(JsonObject) refusal = NULL;

		log.allow = FALSE;
		clawt_mcp_relay_policy_outbound(&policy, line, &to_server,
						&to_client);
		g_assert_null(to_server);
		refusal = parse(to_client);
		g_assert_cmpint(json_object_get_int_member(refusal, "id"), ==, 4);
		g_assert_true(json_object_has_member(refusal, "error"));
	}

	for (i = 0; i < asked->len; i++)
		g_assert_null(strstr(g_ptr_array_index(asked, i), "_call_tool"));
}

/* ── Pages ───────────────────────────────────────────────────────── */

static void
test_pages_are_gathered(void)
{
	g_autoptr(ClawtMcpDirectory) directory = clawt_mcp_directory_new(
		"forge", CLAWT_TOOL_DIRECTORY_AUTO, FALSE, 4, 100000);
	g_autoptr(JsonArray) all = forge_tools();
	g_autoptr(JsonArray) first = json_array_new();
	g_autoptr(JsonArray) second = json_array_new();
	ClawtMcpRelayPolicy policy = { 0 };
	g_autofree gchar *to_server = NULL;
	g_autofree gchar *to_client = NULL;
	g_autofree gchar *page1 = NULL;
	g_autofree gchar *page2 = NULL;
	g_autofree gchar *next = NULL;
	g_autofree gchar *answer = NULL;
	g_autofree gchar *names = NULL;
	g_autoptr(JsonObject) request = NULL;
	g_autoptr(JsonObject) finished = NULL;
	guint i;

	for (i = 0; i < json_array_get_length(all); i++)
		json_array_add_element(i < 3 ? first : second,
				       json_node_copy(json_array_get_element(all,
									      i)));

	policy.directory = directory;

	clawt_mcp_relay_policy_outbound(
		&policy, "{\"jsonrpc\":\"2.0\",\"id\":\"c1\",\"method\":"
		"\"tools/list\"}", &to_server, &to_client);

	/* Page one carries a cursor: the directory asks for page two itself. */
	page1 = list_reply("\"c1\"", first, "after-3");
	clawt_mcp_relay_policy_inbound(&policy, page1, &answer, &next);
	g_assert_null(answer);
	g_assert_nonnull(next);

	request = parse(next);
	g_assert_cmpstr(json_object_get_string_member(request, "method"), ==,
			"tools/list");
	g_assert_cmpstr(json_object_get_string_member(
		json_object_get_object_member(request, "params"), "cursor"), ==,
		"after-3");

	/* Page two finishes it: six tools is past four, so a directory. */
	{
		g_autofree gchar *again = NULL;
		g_autofree gchar *their_id = g_strdup_printf(
			"\"%s\"", json_object_get_string_member(request, "id"));

		page2 = list_reply(their_id, second, NULL);
		clawt_mcp_relay_policy_inbound(&policy, page2, &answer, &again);
		g_assert_null(again);
	}

	g_assert_nonnull(answer);
	finished = parse(answer);

	/* Under the client's id, with no cursor left to follow. */
	g_assert_cmpstr(json_object_get_string_member(finished, "id"), ==, "c1");
	g_assert_false(json_object_has_member(
		json_object_get_object_member(finished, "result"),
		"nextCursor"));

	names = listed_names(answer);
	g_assert_cmpstr(names, ==, "forge_search_tools,forge_describe_tool,"
			"forge_call_tool");
	g_assert_cmpuint(clawt_mcp_directory_get_tool_count(directory), ==, 6);
}

/* ── What clients and the daemon keep ────────────────────────────── */

static void
test_census_round_trip(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("clawt-census-XXXXXX", NULL);
	g_autofree gchar *path = clawt_tool_census_path(dir);
	ClawtToolCensus forge = { (gchar *)"forge", 425, 210000,
				  (gchar *)"issues (40), pulls (30)", 1000 };
	ClawtToolCensus mail = { (gchar *)"mail", 12, 9000, NULL, 2000 };
	g_autoptr(GError) error = NULL;
	g_autoptr(ClawtToolCensus) loaded = NULL;
	g_autoptr(ClawtToolCensus) other = NULL;
	g_autoptr(ClawtToolCensus) absent = NULL;

	g_assert_true(clawt_tool_census_store(path, &forge, &error));
	g_assert_no_error(error);
	g_assert_true(clawt_tool_census_store(path, &mail, &error));
	g_assert_no_error(error);

	/* A second report replaces the first, and keeps the other entry. */
	forge.tools = 430;
	g_assert_true(clawt_tool_census_store(path, &forge, &error));

	loaded = clawt_tool_census_load(path, "forge");
	other = clawt_tool_census_load(path, "mail");
	absent = clawt_tool_census_load(path, "nobody");

	g_assert_nonnull(loaded);
	g_assert_cmpint(loaded->tools, ==, 430);
	g_assert_cmpint(loaded->chars, ==, 210000);
	g_assert_cmpstr(loaded->areas, ==, "issues (40), pulls (30)");
	g_assert_nonnull(other);
	g_assert_cmpint(other->tools, ==, 12);
	g_assert_null(absent);

	clawt_test_remove_tree(dir);
}

static void
test_status_label(void)
{
	g_autofree gchar *unknown = clawt_mcp_directory_status_label(
		"auto", -1, 0, 40, 100000);
	g_autofree gchar *big = clawt_mcp_directory_status_label(
		"auto", 425, 210000, 40, 100000);
	g_autofree gchar *small = clawt_mcp_directory_status_label(
		NULL, 12, 900, 40, 100000);
	g_autofree gchar *never = clawt_mcp_directory_status_label(
		"never", 425, 210000, 40, 100000);

	g_assert_nonnull(strstr(unknown, "unknown"));
	g_assert_nonnull(strstr(big, "425 tools"));
	g_assert_nonnull(strstr(big, "a directory"));
	g_assert_nonnull(strstr(small, "listed whole"));
	g_assert_nonnull(strstr(never, "every tool listed"));
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/mcp-directory/big-by-count", test_big_by_count);
	g_test_add_func("/mcp-directory/big-by-chars", test_big_by_chars);
	g_test_add_func("/mcp-directory/override-modes", test_override_modes);
	g_test_add_func("/mcp-directory/search-ranking", test_search_ranking);
	g_test_add_func("/mcp-directory/permitted-only", test_permitted_only);
	g_test_add_func("/mcp-directory/approval-names-inner-tool",
			test_approval_names_inner_tool);
	g_test_add_func("/mcp-directory/pages-are-gathered",
			test_pages_are_gathered);
	g_test_add_func("/mcp-directory/census-round-trip",
			test_census_round_trip);
	g_test_add_func("/mcp-directory/status-label", test_status_label);

	return g_test_run();
}
