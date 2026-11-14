/*
 * test-delegation-modes.c - Delegating into a thread of its own, asking
 * a question that cannot become work, and a delegated agent's questions
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Five things, each the smallest test that reaches it:
 *
 *   - `thread: new` on clawtilla_delegate: the work gets a room (and so
 *     a libreclaw session) of its own, reported on the task;
 *   - consultations: `consult: true` on clawtilla_ask_agent and `mode:
 *     consult` on clawtilla_delegate withhold exactly the tools that act,
 *     through the tool list, the call and the CLI's own permission hook;
 *   - a decision raised in a delegated turn is put to the delegator
 *     first, who may answer it; the operator still sees it;
 *   - the progress guard: two idle continuations, or three identical
 *     errors, end the work as stalled;
 *   - results for a room whose turn is running are held, and handed over
 *     together when it ends.
 */

#include <clawtilla.h>

#include <gio/gio.h>
#include <glib/gstdio.h>

#include <sqlite3.h>
#include <string.h>
#include <sys/socket.h>

#include "clawt-test-util.h"

/* ── Helpers shared by every section ─────────────────────────────── */

static JsonNode *
call_tool_in(ClawtMcpTools *tools, const gchar *agent_id, const gchar *room,
	const gchar *name, const gchar *arguments_json)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autofree gchar *request = NULL;
	g_autoptr(GError) error = NULL;

	request = g_strdup_printf(
		"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
		"\"params\":{\"name\":\"%s\",\"arguments\":%s}}",
		name, arguments_json);
	g_assert_true(json_parser_load_from_data(parser, request, -1, &error));

	return clawt_mcp_tools_call(tools, agent_id, room,
		json_parser_get_root(parser));
}

static const gchar *
response_text(JsonNode *response, gboolean *out_is_error)
{
	JsonObject *result = json_object_get_object_member(
		json_node_get_object(response), "result");
	JsonArray *content = json_object_get_array_member(result, "content");

	*out_is_error = json_object_get_boolean_member(result, "isError");

	return json_object_get_string_member(
		json_array_get_object_element(content, 0), "text");
}

/* The names a tools/list offers, as a set. */
static GHashTable *
listed_names(ClawtMcpTools *tools, const gchar *agent_id, const gchar *room)
{
	g_autoptr(JsonNode) listing = clawt_mcp_tools_list_in_room(tools,
		agent_id, room);
	JsonArray *array = json_object_get_array_member(
		json_node_get_object(listing), "tools");
	GHashTable *names = g_hash_table_new_full(g_str_hash, g_str_equal,
		g_free, NULL);
	guint i;

	for (i = 0; i < json_array_get_length(array); i++)
		g_hash_table_add(names, g_strdup(json_object_get_string_member(
			json_array_get_object_element(array, i), "name")));

	return names;
}

/* A turn in @room on behalf of @item, begun the way the typing edge does. */
static void
begin_turn_on(ClawtAgent *agent, ClawtMailboxItem *item, const gchar *room)
{
	clawt_agent_deliver_item(agent, item, TRUE);
	clawt_agent_note_typing(agent, room, TRUE);
}

/* ── The message and the item ────────────────────────────────────── */

/*
 * The role copies with the message, the label is one per role, and only
 * a question is carried onto the item: an answer marked for the clients
 * must never make the asker's next turn a consultation.
 */
static void
test_consult_role_and_label(void)
{
	g_autoptr(ClawtMessage) message = clawt_message_new("r", "a", "q");
	g_autoptr(ClawtMessage) copy = NULL;

	g_assert_cmpint(clawt_message_get_consult(message), ==,
		CLAWT_CONSULT_NONE);
	clawt_message_set_consult(message, CLAWT_CONSULT_ASKED);
	copy = clawt_message_copy(message);
	g_assert_cmpint(clawt_message_get_consult(copy), ==, CLAWT_CONSULT_ASKED);

	g_assert_cmpstr(clawt_message_consult_label(CLAWT_CONSULT_ASKED), ==,
		"consultation");
	g_assert_nonnull(clawt_message_consult_label(CLAWT_CONSULT_ANSWERED));
	g_assert_null(clawt_message_consult_label(CLAWT_CONSULT_NONE));
	g_assert_null(clawt_message_consult_label(42));
	g_assert_nonnull(clawt_message_consult_tooltip());
}

/* Stored, so a consultation redelivered after a restart is still one. */
static void
test_consult_survives_a_restart_and_migrates(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("clawt-modes-XXXXXX", NULL);
	g_autofree gchar *path = g_build_filename(dir, "mailbox.db", NULL);
	g_autofree gchar *old_path = g_build_filename(dir, "old.db", NULL);
	g_autofree gchar *id = NULL;

	{
		g_autoptr(ClawtMailbox) mailbox = NULL;
		g_autoptr(ClawtMailboxItem) item = NULL;
		g_autoptr(GError) error = NULL;

		mailbox = clawt_mailbox_new("beta", path, &error);
		g_assert_no_error(error);
		item = clawt_mailbox_item_new("alpha", "beta", "what do you think?");
		clawt_mailbox_item_set_consult(item, TRUE);
		id = clawt_mailbox_post(mailbox, item, &error);
		g_assert_no_error(error);
	}

	{
		g_autoptr(ClawtMailbox) mailbox = NULL;
		g_autoptr(ClawtMailboxItem) item = NULL;
		g_autoptr(GError) error = NULL;

		mailbox = clawt_mailbox_new("beta", path, &error);
		g_assert_no_error(error);
		item = clawt_mailbox_get(mailbox, id);
		g_assert_nonnull(item);
		g_assert_true(clawt_mailbox_item_get_consult(item));
	}

	{
		g_autoptr(ClawtMailbox) mailbox = NULL;
		g_autoptr(ClawtMailboxItem) item = NULL;
		g_autoptr(GError) error = NULL;
		sqlite3 *db = NULL;

		/*
		 * A store from before both 0.20.0's round columns and the
		 * consultation: the two migrations apply one after the other.
		 */
		g_assert_cmpint(sqlite3_open(old_path, &db), ==, SQLITE_OK);
		g_assert_cmpint(sqlite3_exec(db,
			"CREATE TABLE items ("
			"  id TEXT PRIMARY KEY, sender TEXT, recipient TEXT,"
			"  body TEXT NOT NULL, room TEXT, task_id TEXT, reply_to TEXT,"
			"  request_room TEXT, request_origin TEXT, request_task TEXT,"
			"  model TEXT, subject TEXT, idempotency_key TEXT,"
			"  last_error TEXT, priority INTEGER NOT NULL DEFAULT 1,"
			"  state INTEGER NOT NULL DEFAULT 0,"
			"  depth INTEGER NOT NULL DEFAULT 0,"
			"  attempts INTEGER NOT NULL DEFAULT 0,"
			"  invites_reply INTEGER NOT NULL DEFAULT 1,"
			"  composed INTEGER NOT NULL DEFAULT 0,"
			"  one_way INTEGER NOT NULL DEFAULT 0,"
			"  created_at INTEGER NOT NULL,"
			"  not_before INTEGER NOT NULL DEFAULT 0,"
			"  expires_at INTEGER NOT NULL DEFAULT 0,"
			"  lease_expires_at INTEGER NOT NULL DEFAULT 0);"
			"INSERT INTO items (id, sender, recipient, body, state, created_at)"
			"  VALUES ('old-1', 'alpha', 'beta', 'from before', 0, 1);",
			NULL, NULL, NULL), ==, SQLITE_OK);
		sqlite3_close_v2(db);

		mailbox = clawt_mailbox_new("beta", old_path, &error);
		g_assert_no_error(error);
		item = clawt_mailbox_get(mailbox, "old-1");
		g_assert_nonnull(item);
		g_assert_false(clawt_mailbox_item_get_consult(item));
		g_assert_cmpuint(clawt_mailbox_item_get_round(item), ==, 0);
		g_assert_null(clawt_mailbox_item_get_chain(item));

		/* And it takes new work carrying all three. */
		{
			g_autoptr(ClawtMailboxItem) fresh = clawt_mailbox_item_new(
				"alpha", "beta", "is it sound?");
			g_autoptr(ClawtMailboxItem) back = NULL;
			g_autofree gchar *fresh_id = NULL;

			clawt_mailbox_item_set_consult(fresh, TRUE);
			clawt_mailbox_item_set_round_chain(fresh, 3, "alpha");
			fresh_id = clawt_mailbox_post(mailbox, fresh, &error);
			g_assert_no_error(error);
			back = clawt_mailbox_get(mailbox, fresh_id);
			g_assert_true(clawt_mailbox_item_get_consult(back));
			g_assert_cmpuint(clawt_mailbox_item_get_round(back), ==, 3);
			g_assert_cmpstr(clawt_mailbox_item_get_chain(back), ==, "alpha");
		}
	}

	clawt_test_remove_tree(dir);
}

/* ── The gate ────────────────────────────────────────────────────── */

typedef struct {
	gchar              *dir;
	ClawtConfig        *config;
	ClawtAgentManager  *agents;
	ClawtRoomManager   *rooms;
	ClawtMailboxRouter *router;
	ClawtTaskManager   *tasks;
	ClawtMcpTools      *tools;
	GHashTable         *far_ends;   /* agent id -> GSocket */
} Fixture;

static void
fixture_setup(Fixture *fixture)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *yaml = NULL;

	fixture->dir = g_dir_make_tmp("clawt-modes-XXXXXX", NULL);

	/* All three pinned, or a fixture scaffolds into the real fleet. */
	yaml = g_strdup_printf("daemon:\n"
		"  state_dir: \"%s\"\n"
		"  socket: \"%s/daemon.sock\"\n"
		"defaults:\n  workspace_root: \"%s/agents\"\n"
		"agents:\n"
		"  - id: chief\n    chief_of_staff: true\n"
		"  - id: beta\n"
		"  - id: gamma\n",
		fixture->dir, fixture->dir, fixture->dir);

	fixture->config = clawt_config_load_from_string(yaml, &error);
	g_assert_no_error(error);

	fixture->agents = clawt_agent_manager_new(fixture->config);
	clawt_agent_manager_load(fixture->agents, NULL);
	fixture->rooms = clawt_room_manager_new(NULL);
	fixture->router = clawt_mailbox_router_new(fixture->agents,
		fixture->rooms, NULL);
	fixture->tasks = clawt_task_manager_new();
	fixture->tools = clawt_mcp_tools_new(fixture->agents, fixture->tasks,
		NULL);
	clawt_mcp_tools_set_room_manager(fixture->tools, fixture->rooms);
	fixture->far_ends = g_hash_table_new_full(g_str_hash, g_str_equal,
		g_free, g_object_unref);
}

static void
fixture_teardown(Fixture *fixture)
{
	g_clear_pointer(&fixture->far_ends, g_hash_table_unref);
	g_clear_object(&fixture->tools);
	g_clear_object(&fixture->tasks);
	g_clear_object(&fixture->router);
	g_clear_object(&fixture->rooms);
	g_clear_object(&fixture->agents);
	g_clear_object(&fixture->config);

	if (fixture->dir != NULL)
		clawt_test_remove_tree(fixture->dir);
	g_clear_pointer(&fixture->dir, g_free);
}

/*
 * A consultation withholds exactly the acting tools: what the ordinary
 * turn is offered, less every tool clawt_mcp_tools_consult_withholds()
 * names -- and nothing else.  The delegator's tools are the ones at
 * stake, so the turn is the chief's.  Two rooms, because the state is
 * per turn: the chief's ordinary turn in another room keeps everything.
 */
static void
test_consult_withholds_exactly_the_acting_tools(void)
{
	Fixture fixture = { 0 };
	ClawtAgent *chief;
	g_autoptr(ClawtMailboxItem) asked = NULL;
	g_autoptr(ClawtMailboxItem) ordinary = NULL;
	g_autoptr(GHashTable) before = NULL;
	g_autoptr(GHashTable) during = NULL;
	g_autoptr(GHashTable) elsewhere = NULL;
	g_autoptr(GHashTable) folded = NULL;
	GHashTableIter iter;
	gpointer name;
	guint withheld = 0;

	fixture_setup(&fixture);
	chief = clawt_agent_manager_get(fixture.agents, "chief");

	before = listed_names(fixture.tools, "chief", "consult-room");
	g_assert_true(g_hash_table_contains(before, "clawtilla_delegate"));

	asked = clawt_mailbox_item_new("beta", "chief", "what do you think?");
	clawt_mailbox_item_set_room(asked, "consult-room");
	clawt_mailbox_item_set_consult(asked, TRUE);
	begin_turn_on(chief, asked, "consult-room");

	ordinary = clawt_mailbox_item_new("user", "chief", "do the thing");
	clawt_mailbox_item_set_room(ordinary, "work-room");
	begin_turn_on(chief, ordinary, "work-room");

	g_assert_true(clawt_mcp_tools_turn_is_consult(fixture.tools, "chief",
		"consult-room"));
	g_assert_false(clawt_mcp_tools_turn_is_consult(fixture.tools, "chief",
		"work-room"));

	during = listed_names(fixture.tools, "chief", "consult-room");
	elsewhere = listed_names(fixture.tools, "chief", "work-room");
	folded = listed_names(fixture.tools, "chief", NULL);

	g_hash_table_iter_init(&iter, before);
	while (g_hash_table_iter_next(&iter, &name, NULL)) {
		ClawtActionClass action = CLAWT_ACTION_EXTERNAL;
		gboolean acts;

		g_assert_true(clawt_mcp_tools_action_class(name, &action));
		acts = clawt_mcp_tools_consult_withholds(name, action);

		g_assert_cmpint(g_hash_table_contains(during, name), ==, !acts);
		/* The other room's ordinary turn keeps everything... */
		g_assert_true(g_hash_table_contains(elsewhere, name));
		/* ...and a call that names no room folds towards withholding. */
		g_assert_cmpint(g_hash_table_contains(folded, name), ==, !acts);
		withheld += acts ? 1 : 0;
	}
	g_assert_cmpuint(g_hash_table_size(during), ==,
		g_hash_table_size(before) - withheld);

	/* The acting tools by name, and the ones an answer needs. */
	g_assert_false(g_hash_table_contains(during, "clawtilla_delegate"));
	g_assert_false(g_hash_table_contains(during, "clawtilla_message_agent"));
	g_assert_false(g_hash_table_contains(during, "clawtilla_ask_agent"));
	g_assert_false(g_hash_table_contains(during, "clawtilla_post_room"));
	g_assert_false(g_hash_table_contains(during, "clawtilla_message_user"));
	g_assert_true(g_hash_table_contains(during, "clawtilla_task_complete"));
	g_assert_true(g_hash_table_contains(during, "clawtilla_list_agents"));
	g_assert_true(g_hash_table_contains(during, "clawtilla_room_history"));

	/* Tools this fleet does not offer, and the CLI's and connectors' own. */
	g_assert_true(clawt_mcp_tools_consult_withholds("clawtilla_computer_exec",
		CLAWT_ACTION_READ));
	g_assert_true(clawt_mcp_tools_consult_withholds("Bash", CLAWT_ACTION_EXEC));
	g_assert_true(clawt_mcp_tools_consult_withholds("mcp__crm__update",
		CLAWT_ACTION_EXTERNAL));
	g_assert_false(clawt_mcp_tools_consult_withholds("Read", CLAWT_ACTION_READ));

	/* A call that names one anyway is refused, with the rule. */
	{
		g_autoptr(JsonNode) response = call_tool_in(fixture.tools, "chief",
			"consult-room", "clawtilla_message_agent",
			"{\"agent_id\":\"beta\",\"body\":\"go and do it\"}");
		gboolean is_error = FALSE;
		const gchar *text = response_text(response, &is_error);

		g_assert_true(is_error);
		g_assert_nonnull(strstr(text, "consultation"));
	}

	/*
	 * The turn ends: a finished consultation withholds nothing, so the
	 * tools written into TOOLS.org at a start do not depend on it.
	 */
	clawt_agent_note_typing(chief, "consult-room", FALSE);
	clawt_agent_note_typing(chief, "work-room", FALSE);
	g_assert_false(clawt_mcp_tools_turn_is_consult(fixture.tools, "chief",
		NULL));

	fixture_teardown(&fixture);
}

/*
 * A CLI asks for its tools before the typing frame that begins its turn
 * reaches the daemon; the waiting delivery answers for that turn.  And a
 * merged turn with one consulted part is a consultation.
 */
static void
test_consult_before_the_turn_and_when_merged(void)
{
	Fixture fixture = { 0 };
	ClawtAgent *beta;
	g_autoptr(ClawtMailboxItem) asked = NULL;
	g_autoptr(ClawtMailboxItem) merged = NULL;
	g_autoptr(ClawtMailboxItem) plain = NULL;
	g_autoptr(GPtrArray) parts = g_ptr_array_new();
	gboolean replies[2] = { FALSE, FALSE };
	gboolean peers[2] = { TRUE, TRUE };

	fixture_setup(&fixture);
	beta = clawt_agent_manager_get(fixture.agents, "beta");

	asked = clawt_mailbox_item_new("chief", "beta", "is this sound?");
	clawt_mailbox_item_set_room(asked, "r1");
	clawt_mailbox_item_set_consult(asked, TRUE);
	clawt_agent_deliver_item(beta, asked, TRUE);

	g_assert_true(clawt_agent_get_turn_consult_in(beta, "r1"));
	g_assert_false(clawt_agent_get_turn_consult_in(beta, "r2"));

	plain = clawt_mailbox_item_new("gamma", "beta", "fyi");
	clawt_mailbox_item_set_room(plain, "r2");
	g_ptr_array_add(parts, plain);
	g_ptr_array_add(parts, asked);
	merged = clawt_mailbox_item_new("gamma", "beta", "two parts");
	clawt_mailbox_item_set_room(merged, "r2");
	clawt_agent_deliver_merged(beta, merged, parts, replies, peers);
	clawt_agent_note_typing(beta, "r2", TRUE);
	g_assert_true(clawt_agent_get_turn_consult_in(beta, "r2"));

	fixture_teardown(&fixture);
}

typedef struct {
	gint   calls;
	guint  flags;
	gchar *target;
} Delivered;

static gboolean
record_deliver(const gchar *from_agent, const gchar *target, const gchar *body,
	const gchar *task_id, gint depth, ClawtPriority priority,
	const gchar *turn_room, guint flags, gpointer user_data, GError **error)
{
	Delivered *seen = user_data;

	(void)from_agent; (void)body; (void)task_id; (void)depth;
	(void)priority; (void)turn_room; (void)error;

	seen->calls++;
	seen->flags = flags;
	g_free(seen->target);
	seen->target = g_strdup(target);
	return TRUE;
}

static gboolean
old_deliver(const gchar *from_agent, const gchar *target, const gchar *body,
	const gchar *task_id, gint depth, ClawtPriority priority,
	const gchar *turn_room, gpointer user_data, GError **error)
{
	Delivered *seen = user_data;

	(void)from_agent; (void)target; (void)body; (void)task_id;
	(void)depth; (void)priority; (void)turn_room; (void)error;

	seen->calls++;
	return TRUE;
}

/*
 * The arguments: `consult` reaches the hook as a flag, asking for an
 * answer and for none at once is refused, a value of `thread` or `mode`
 * nobody knows is refused before anything is made, and an older hook
 * that cannot carry a consultation refuses it rather than sending a
 * question whose turn could act.
 */
static void
test_the_arguments(void)
{
	Fixture fixture = { 0 };
	Delivered seen = { 0 };
	gboolean is_error = FALSE;
	const gchar *text;
	g_autoptr(GPtrArray) tasks = NULL;

	fixture_setup(&fixture);
	clawt_mcp_tools_set_deliver_reply_func(fixture.tools, record_deliver,
		&seen, NULL);

	{
		g_autoptr(JsonNode) response = call_tool_in(fixture.tools, "chief",
			NULL, "clawtilla_ask_agent",
			"{\"agent_id\":\"beta\",\"message\":\"is it sound?\","
			"\"consult\":true}");

		text = response_text(response, &is_error);
		g_assert_false(is_error);
		g_assert_nonnull(strstr(text, "consultation"));
		g_assert_cmpint(seen.calls, ==, 1);
		g_assert_cmpuint(seen.flags, ==, CLAWT_MCP_DELIVER_CONSULT);
	}

	{
		g_autoptr(JsonNode) response = call_tool_in(fixture.tools, "chief",
			NULL, "clawtilla_ask_agent",
			"{\"agent_id\":\"beta\",\"message\":\"q\",\"consult\":true,"
			"\"reply\":\"none\"}");

		response_text(response, &is_error);
		g_assert_true(is_error);
		g_assert_cmpint(seen.calls, ==, 1);
	}

	{
		g_autoptr(JsonNode) response = call_tool_in(fixture.tools, "chief",
			NULL, "clawtilla_delegate",
			"{\"agent_id\":\"beta\",\"task\":\"x\",\"thread\":\"fresh\"}");

		text = response_text(response, &is_error);
		g_assert_true(is_error);
		g_assert_nonnull(strstr(text, "standing or new"));
	}

	{
		g_autoptr(JsonNode) response = call_tool_in(fixture.tools, "chief",
			NULL, "clawtilla_delegate",
			"{\"agent_id\":\"beta\",\"task\":\"x\",\"mode\":\"advise\"}");

		text = response_text(response, &is_error);
		g_assert_true(is_error);
		g_assert_nonnull(strstr(text, "work or consult"));
	}

	tasks = clawt_task_manager_list(fixture.tasks, NULL, TRUE);
	g_assert_cmpuint(tasks->len, ==, 0);
	g_assert_cmpint(seen.calls, ==, 1);

	clawt_mcp_tools_set_deliver_turn_func(fixture.tools, old_deliver, &seen,
		NULL);
	{
		g_autoptr(JsonNode) response = call_tool_in(fixture.tools, "chief",
			NULL, "clawtilla_ask_agent",
			"{\"agent_id\":\"beta\",\"message\":\"q\",\"consult\":true}");

		response_text(response, &is_error);
		g_assert_true(is_error);
		g_assert_cmpint(seen.calls, ==, 1);
	}

	g_free(seen.target);
	fixture_teardown(&fixture);
}

/*
 * `thread: new` without a daemon: the tools make the room, record it on
 * the task, deliver into it, and say where it is.
 */
static void
test_thread_new_makes_and_reports_a_room(void)
{
	Fixture fixture = { 0 };
	Delivered seen = { 0 };
	gboolean is_error = TRUE;
	const gchar *text;
	g_autoptr(GPtrArray) tasks = NULL;
	ClawtTask *task;
	ClawtRoom *room;
	g_autofree gchar *expected = NULL;

	fixture_setup(&fixture);
	clawt_mcp_tools_set_deliver_reply_func(fixture.tools, record_deliver,
		&seen, NULL);

	{
		g_autoptr(JsonNode) response = call_tool_in(fixture.tools, "chief",
			NULL, "clawtilla_delegate",
			"{\"agent_id\":\"beta\",\"task\":\"audit the build\","
			"\"thread\":\"new\",\"mode\":\"consult\"}");

		text = response_text(response, &is_error);
		g_assert_false(is_error);

		tasks = clawt_task_manager_list(fixture.tasks, NULL, TRUE);
		g_assert_cmpuint(tasks->len, ==, 1);
		task = g_ptr_array_index(tasks, 0);
		expected = clawt_room_manager_task_room_id(clawt_task_get_id(task));

		g_assert_cmpstr(clawt_task_get_thread_room(task), ==, expected);
		g_assert_true(clawt_task_get_consult(task));
		g_assert_nonnull(strstr(text, expected));
		g_assert_cmpstr(seen.target, ==, expected);
	}

	room = clawt_room_manager_get(fixture.rooms, expected);
	g_assert_nonnull(room);
	g_assert_true(clawt_room_has_member(room, "chief"));
	g_assert_true(clawt_room_has_member(room, "beta"));
	g_assert_false(clawt_room_is_group(room));

	g_free(seen.target);
	fixture_teardown(&fixture);
}

/* ── Holding results for a busy room ─────────────────────────────── */

static void
give_agent_a_link(Fixture *fixture, const gchar *agent_id)
{
	ClawtAgent *agent = clawt_agent_manager_get(fixture->agents, agent_id);
	g_autoptr(GSocket) near_end = NULL;
	g_autoptr(GSocketConnection) connection = NULL;
	g_autoptr(ClawtLink) link = NULL;
	g_autoptr(GError) error = NULL;
	GSocket *far_end;
	int fds[2];

	g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), ==, 0);
	near_end = g_socket_new_from_fd(fds[0], &error);
	g_assert_no_error(error);
	far_end = g_socket_new_from_fd(fds[1], &error);
	g_assert_no_error(error);
	g_hash_table_replace(fixture->far_ends, g_strdup(agent_id), far_end);

	connection = g_socket_connection_factory_create_connection(near_end);
	link = clawt_link_new(connection);
	clawt_agent_set_link(agent, link);
}

static gchar *
read_all(Fixture *fixture, const gchar *agent_id)
{
	GSocket *far_end = g_hash_table_lookup(fixture->far_ends, agent_id);
	g_autoptr(GString) out = g_string_new(NULL);
	gchar buffer[16384];

	g_socket_set_blocking(far_end, FALSE);

	for (;;) {
		gssize got = g_socket_receive(far_end, buffer, sizeof(buffer),
			NULL, NULL);

		if (got <= 0)
			break;
		g_string_append_len(out, buffer, got);
	}

	return g_string_free(g_steal_pointer(&out), FALSE);
}

static void
send_notice(Fixture *fixture, const gchar *room, const gchar *agent_id,
	const gchar *task_id, const gchar *body)
{
	g_autoptr(ClawtMessage) message =
		clawt_message_new(room, CLAWT_SYSTEM_SENDER, body);
	g_autoptr(GError) error = NULL;

	clawt_message_set_task_id(message, task_id);
	clawt_message_set_invites_reply(message, FALSE);
	clawt_message_set_only_for(message, agent_id);
	g_assert_cmpint(clawt_mailbox_router_send(fixture->router, message,
		&error), ==, 1);
	g_assert_no_error(error);
}

/*
 * A connected chief busy in its operator's room: two settle notices for
 * that room wait, a notice for another room and a person's message go
 * straight through, and the turn ending hands the two over as one.
 * Before, a connected agent took every result at the socket and
 * libreclaw queued a turn for each, so the merge could only ever see a
 * mailbox that was full because nobody was connected.
 */
static void
test_results_wait_for_the_busy_room(void)
{
	Fixture fixture = { 0 };
	ClawtAgent *chief;
	ClawtRoom *operator_room;
	ClawtRoom *other;
	g_autofree gchar *first = NULL;
	g_autofree gchar *second = NULL;

	fixture_setup(&fixture);
	chief = clawt_agent_manager_get(fixture.agents, "chief");
	operator_room = clawt_room_manager_get_direct(fixture.rooms, "chief",
		"user");
	other = clawt_room_manager_get_direct(fixture.rooms, "chief", "beta");
	give_agent_a_link(&fixture, "chief");

	clawt_agent_note_typing(chief, clawt_room_get_id(operator_room), TRUE);

	send_notice(&fixture, clawt_room_get_id(operator_room), "chief", "t-1",
		"[clawtilla] Task t-1 is finished.");
	send_notice(&fixture, clawt_room_get_id(operator_room), "chief", "t-2",
		"[clawtilla] Task t-2 is finished.");
	send_notice(&fixture, clawt_room_get_id(other), "chief", "t-3",
		"[clawtilla] Task t-3 is finished.");

	first = read_all(&fixture, "chief");
	g_assert_nonnull(strstr(first, "Task t-3 is finished"));
	g_assert_null(strstr(first, "Task t-1 is finished"));
	g_assert_cmpuint(clawt_mailbox_depth(clawt_agent_get_mailbox(chief)), ==,
		2);

	/* A person's message into the busy room is not held. */
	{
		g_autoptr(ClawtMessage) message = clawt_message_new(
			clawt_room_get_id(operator_room), "user", "and another thing");
		g_autoptr(GError) error = NULL;

		g_assert_cmpint(clawt_mailbox_router_send(fixture.router, message,
			&error), ==, 1);
		g_assert_no_error(error);
	}
	g_free(first);
	first = read_all(&fixture, "chief");
	g_assert_nonnull(strstr(first, "and another thing"));
	g_assert_cmpuint(clawt_mailbox_depth(clawt_agent_get_mailbox(chief)), ==,
		2);

	/* The turn ends: one delivery, both results in it. */
	clawt_agent_note_typing(chief, clawt_room_get_id(operator_room), FALSE);
	g_assert_cmpuint(clawt_mailbox_router_drain(fixture.router, "chief"), ==,
		2);
	second = read_all(&fixture, "chief");
	g_assert_cmpuint(clawt_test_count_substrings(second,
		"results for you arrived in room"), ==, 1);
	g_assert_nonnull(strstr(second, "Task t-1 is finished"));
	g_assert_nonnull(strstr(second, "Task t-2 is finished"));

	fixture_teardown(&fixture);
}

/* ── With rounds in a group room ─────────────────────────────────── */

/* A three-member group; @mentions decides whether it is a mention room. */
static ClawtRoom *
make_group(Fixture *fixture, const gchar *room_id, gboolean mentions)
{
	g_autoptr(GError) error = NULL;
	ClawtRoom *room = clawt_room_manager_create(fixture->rooms, room_id,
		NULL, &error);

	g_assert_no_error(error);
	clawt_room_add_member(room, "chief");
	clawt_room_add_member(room, "beta");
	clawt_room_add_member(room, "gamma");
	clawt_room_set_require_mention(room, mentions);
	return room;
}

static void
person_says(Fixture *fixture, const gchar *room_id, const gchar *body)
{
	g_autoptr(GError) error = NULL;

	g_assert_cmpint(clawt_mailbox_router_send_to(fixture->router, "user",
		room_id, body, NULL, 0, &error), >=, 0);
	g_assert_no_error(error);
}

/*
 * A consultation asked inside a round of a mention room: the item
 * carries both the round's thread and the consultation, the member is
 * told both the round rule and that it is a consultation, and its turn
 * withholds the acting tools while an ordinary turn in another room
 * does not.  The two are independent fields on one item; a merge of
 * either onto the other would lose one of them.
 */
static void
test_consultation_inside_a_round(void)
{
	Fixture fixture = { 0 };
	ClawtAgent *chief;
	ClawtAgent *beta;
	g_autoptr(ClawtMessage) asked = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(ClawtMailboxItem) item = NULL;
	g_autofree gchar *handed = NULL;

	fixture_setup(&fixture);
	chief = clawt_agent_manager_get(fixture.agents, "chief");
	beta = clawt_agent_manager_get(fixture.agents, "beta");
	make_group(&fixture, "standup", TRUE);
	give_agent_a_link(&fixture, "beta");

	/* A person opens round 1 by naming the chief, who takes its turn. */
	person_says(&fixture, "standup", "@chief what is the plan?");
	item = clawt_mailbox_lease(clawt_agent_get_mailbox(chief), 0);
	g_assert_nonnull(item);
	g_assert_cmpuint(clawt_mailbox_item_get_round(item), ==, 1);
	begin_turn_on(chief, item, "standup");

	/* The chief consults beta inside that round. */
	asked = clawt_message_new("standup", "chief",
		"@beta is the migration order safe?");
	clawt_message_set_consult(asked, CLAWT_CONSULT_ASKED);
	clawt_message_set_depth(asked, 1);
	g_assert_cmpint(clawt_mailbox_router_send(fixture.router, asked,
		&error), ==, 1);
	g_assert_no_error(error);

	handed = read_all(&fixture, "beta");
	g_assert_nonnull(strstr(handed, "This is a consultation"));
	g_assert_nonnull(strstr(handed, "round 1"));

	/* Delivered, so the turn state is beta's: both fields arrived. */
	clawt_agent_note_typing(beta, "standup", TRUE);
	g_assert_true(clawt_agent_get_turn_consult_in(beta, "standup"));
	{
		ClawtMailboxItem *input = clawt_agent_get_turn_input_in(beta,
			"standup");

		g_assert_nonnull(input);
		g_assert_true(clawt_mailbox_item_get_consult(input));
		g_assert_cmpuint(clawt_mailbox_item_get_round(input), ==, 1);
		g_assert_nonnull(strstr(clawt_mailbox_item_get_chain(input),
			"chief"));
	}

	{
		g_autoptr(GHashTable) offered = listed_names(fixture.tools, "beta",
			"standup");

		g_assert_false(g_hash_table_contains(offered,
			"clawtilla_post_room"));
		g_assert_true(g_hash_table_contains(offered,
			"clawtilla_room_history"));
	}

	/* The room's record says what it was, for the clients. */
	{
		g_autoptr(GPtrArray) history = clawt_room_get_history(
			clawt_room_manager_get(fixture.rooms, "standup"), 1);
		ClawtMessage *last = g_ptr_array_index(history, 0);

		g_assert_cmpint(clawt_message_get_consult(last), ==,
			CLAWT_CONSULT_ASKED);
		g_assert_cmpuint(clawt_message_get_round(last), ==, 1);
	}

	fixture_teardown(&fixture);
}

/*
 * Results held for a busy group room keep their round: two answers in
 * round 1 of a room without the mention rule wait while the chief's
 * turn there runs, and the one delivery that follows describes round 1
 * with both answerers in its thread -- so the chief's own reply
 * continues that round rather than opening a thread of its own.
 */
static void
test_held_results_keep_their_round(void)
{
	Fixture fixture = { 0 };
	ClawtAgent *chief;
	g_autofree gchar *first = NULL;
	g_autofree gchar *second = NULL;
	const gchar *answerers[] = { "beta", "gamma" };
	guint i;

	fixture_setup(&fixture);
	chief = clawt_agent_manager_get(fixture.agents, "chief");
	make_group(&fixture, "huddle", FALSE);
	give_agent_a_link(&fixture, "chief");

	/* Round 1, opened by a person; the chief's turn is running in it. */
	person_says(&fixture, "huddle", "where are we on the release?");
	g_free(read_all(&fixture, "chief"));
	g_assert_cmpuint(clawt_room_get_round(
		clawt_room_manager_get(fixture.rooms, "huddle")), ==, 1);
	clawt_agent_note_typing(chief, "huddle", TRUE);

	/* Two answers in that round arrive while it runs. */
	for (i = 0; i < G_N_ELEMENTS(answerers); i++) {
		g_autoptr(ClawtMessage) answer = clawt_message_new("huddle",
			answerers[i], answerers[i][0] == 'b' ? "build is green"
			                                     : "docs are done");
		g_autoptr(ClawtMailboxItem) item = NULL;
		g_autoptr(GError) error = NULL;
		ClawtAgent *who = clawt_agent_manager_get(fixture.agents,
			answerers[i]);

		/*
		 * Each answers from the turn the round's opening delivered to
		 * it: unlinked, so the delivery is in its mailbox to take.
		 */
		item = clawt_mailbox_lease(clawt_agent_get_mailbox(who), 0);
		g_assert_nonnull(item);
		g_assert_cmpuint(clawt_mailbox_item_get_round(item), ==, 1);
		begin_turn_on(who, item, "huddle");

		clawt_message_set_invites_reply(answer, FALSE);
		clawt_message_set_depth(answer, 1);
		g_assert_cmpint(clawt_mailbox_router_send(fixture.router, answer,
			&error), >=, 0);
		g_assert_no_error(error);
	}

	first = read_all(&fixture, "chief");
	g_assert_null(strstr(first, "build is green"));
	g_assert_cmpuint(clawt_mailbox_depth(clawt_agent_get_mailbox(chief)), ==,
		2);

	/* The turn ends: one delivery, round 1, both answerers threaded. */
	clawt_agent_note_typing(chief, "huddle", FALSE);
	g_assert_cmpuint(clawt_mailbox_router_drain(fixture.router, "chief"), ==,
		2);
	second = read_all(&fixture, "chief");
	g_assert_cmpuint(clawt_test_count_substrings(second,
		"results for you arrived in room"), ==, 1);

	clawt_agent_note_typing(chief, "huddle", TRUE);
	{
		ClawtMailboxItem *input = clawt_agent_get_turn_input_in(chief,
			"huddle");

		g_assert_nonnull(input);
		g_assert_cmpuint(clawt_mailbox_item_get_round(input), ==, 1);
		g_assert_nonnull(strstr(clawt_mailbox_item_get_chain(input),
			"beta"));
		g_assert_nonnull(strstr(clawt_mailbox_item_get_chain(input),
			"gamma"));
	}

	fixture_teardown(&fixture);
}

/* ── The progress guard ──────────────────────────────────────────── */

static ClawtTask *
running_task(ClawtTaskManager *tasks, const gchar *parent_id)
{
	g_autoptr(GError) error = NULL;
	ClawtTask *task = clawt_task_manager_create(tasks, "chief", "beta",
		"work", parent_id, &error);

	g_assert_no_error(error);
	clawt_task_manager_start(tasks, clawt_task_get_id(task));
	return task;
}

/*
 * Two continuations with no successful call stall the task; one with
 * work in between resets the count; an unobserved turn is neither; and
 * somebody else's turn in the thread does not count.
 */
static void
test_idle_continuations_stall(void)
{
	g_autoptr(ClawtTaskManager) tasks = clawt_task_manager_new();
	ClawtTask *task = running_task(tasks, NULL);
	const gchar *id = clawt_task_get_id(task);

	g_assert_false(clawt_task_manager_note_continuation(tasks, id, "beta",
		TRUE, 0, NULL));
	g_assert_false(clawt_task_manager_note_continuation(tasks, id, "beta",
		TRUE, 3, NULL));
	g_assert_false(clawt_task_manager_note_continuation(tasks, id, "beta",
		TRUE, 0, NULL));
	g_assert_false(clawt_task_manager_note_continuation(tasks, id, "beta",
		FALSE, 0, NULL));
	g_assert_false(clawt_task_manager_note_continuation(tasks, id, "chief",
		TRUE, 0, NULL));
	g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_RUNNING);

	g_assert_true(clawt_task_manager_note_continuation(tasks, id, "beta",
		TRUE, 0, NULL));
	g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_STALLED);
	g_assert_nonnull(strstr(clawt_task_get_reason(task),
		"2 continuations"));

	/* An ended task has nothing left to guard. */
	g_assert_false(clawt_task_manager_note_continuation(tasks, id, "beta",
		TRUE, 0, NULL));
}

/* Three identical errors in a row stall it, even with work between. */
static void
test_same_error_three_times_stalls(void)
{
	g_autoptr(ClawtTaskManager) tasks = clawt_task_manager_new();
	ClawtTask *task = running_task(tasks, NULL);
	const gchar *id = clawt_task_get_id(task);

	g_assert_false(clawt_task_manager_note_continuation(tasks, id, "beta",
		TRUE, 1, "Bash: permission denied"));
	g_assert_false(clawt_task_manager_note_continuation(tasks, id, "beta",
		TRUE, 1, "Bash: no such file"));
	g_assert_false(clawt_task_manager_note_continuation(tasks, id, "beta",
		TRUE, 1, "Bash: no such file"));
	g_assert_true(clawt_task_manager_note_continuation(tasks, id, "beta",
		TRUE, 1, "Bash: no such file"));
	g_assert_cmpint(clawt_task_get_state(task), ==, CLAWT_TASK_STALLED);
	g_assert_nonnull(strstr(clawt_task_get_reason(task), "no such file"));
}

/*
 * A parent woken by each child's settle notice is waiting, not idling:
 * it is never a strike, and it clears the record.
 */
static void
test_waiting_on_children_is_not_a_strike(void)
{
	g_autoptr(ClawtTaskManager) tasks = clawt_task_manager_new();
	ClawtTask *parent = running_task(tasks, NULL);
	const gchar *id = clawt_task_get_id(parent);
	ClawtTask *child;

	g_assert_false(clawt_task_manager_note_continuation(tasks, id, "beta",
		TRUE, 0, NULL));

	child = running_task(tasks, id);
	g_assert_false(clawt_task_manager_note_continuation(tasks, id, "beta",
		TRUE, 0, NULL));
	g_assert_false(clawt_task_manager_note_continuation(tasks, id, "beta",
		TRUE, 0, NULL));
	g_assert_cmpint(clawt_task_get_state(parent), ==, CLAWT_TASK_RUNNING);

	clawt_task_manager_complete(tasks, clawt_task_get_id(child), "done");

	/* The record was cleared while waiting: one idle turn is not two. */
	g_assert_false(clawt_task_manager_note_continuation(tasks, id, "beta",
		TRUE, 0, NULL));
	g_assert_cmpint(clawt_task_get_state(parent), ==, CLAWT_TASK_RUNNING);
}

/* The evidence a turn carries: a progress note is observed, not work. */
static void
test_turn_evidence(void)
{
	Fixture fixture = { 0 };
	ClawtAgent *beta;
	g_autoptr(ClawtMailboxItem) item = NULL;
	guint work = 99;
	const gchar *error = NULL;

	fixture_setup(&fixture);
	beta = clawt_agent_manager_get(fixture.agents, "beta");
	item = clawt_mailbox_item_new("chief", "beta", "go on");
	clawt_mailbox_item_set_room(item, "r");
	begin_turn_on(beta, item, "r");

	g_assert_false(clawt_agent_get_turn_evidence_in(beta, "r", &work, &error));

	clawt_agent_note_turn_work(beta, "r", FALSE, NULL);
	g_assert_true(clawt_agent_get_turn_evidence_in(beta, "r", &work, &error));
	g_assert_cmpuint(work, ==, 0);
	g_assert_null(error);

	clawt_agent_note_turn_work(beta, NULL, FALSE, "Bash: exit 1");
	clawt_agent_note_turn_work(beta, "r", TRUE, NULL);
	g_assert_true(clawt_agent_get_turn_evidence_in(beta, "r", &work, &error));
	g_assert_cmpuint(work, ==, 1);
	g_assert_cmpstr(error, ==, "Bash: exit 1");

	fixture_teardown(&fixture);
}

/* ── With a daemon: decisions, and the delivered consultation ────── */

typedef struct {
	gchar        *dir;
	gchar        *config_path;
	ClawtDaemon  *daemon;
	GMainContext *context;
} Daemon;

static void
daemon_setup(Daemon *fixture)
{
	g_autofree gchar *yaml = NULL;
	g_autoptr(GError) error = NULL;

	fixture->dir = g_dir_make_tmp("clawt-modes-XXXXXX", NULL);
	fixture->config_path = g_build_filename(fixture->dir, "config.yaml", NULL);

	/* Every path a daemon writes for itself, pinned inside the fixture. */
	yaml = g_strdup_printf(
		"daemon:\n"
		"  tailscale: false\n"
		"  state_dir: \"%s/state\"\n"
		"  socket: \"%s/daemon.sock\"\n"
		"  automation_dir: \"%s/pods\"\n"
		"skills:\n  dir: \"%s/skills\"\n"
		"teams:\n  - id: build\n"
		"agents:\n"
		"  - id: chief\n    chief_of_staff: true\n"
		"  - id: beta\n"
		"  - id: gamma\n    team: build\n    team_role: lead\n"
		"defaults:\n  workspace_root: \"%s/agents\"\n",
		fixture->dir, fixture->dir, fixture->dir, fixture->dir,
		fixture->dir);
	g_file_set_contents(fixture->config_path, yaml, -1, &error);
	g_assert_no_error(error);

	fixture->context = g_main_context_new();
	fixture->daemon = clawt_daemon_new(fixture->config_path, fixture->context);
	{
		g_autoptr(GError) start_error = NULL;

		clawt_daemon_start(fixture->daemon, &start_error);
		g_assert_no_error(start_error);
	}
}

static void
daemon_teardown(Daemon *fixture)
{
	clawt_daemon_stop(fixture->daemon);
	g_clear_object(&fixture->daemon);

	while (g_main_context_iteration(fixture->context, FALSE))
		;
	g_main_context_unref(fixture->context);

	clawt_test_remove_tree(fixture->dir);
	g_free(fixture->dir);
	g_free(fixture->config_path);
}

static ClawtAgent *
daemon_agent(Daemon *fixture, const gchar *id)
{
	return clawt_agent_manager_get(clawt_daemon_get_agents(fixture->daemon),
		id);
}

/* What is waiting in @agent_id's mailbox, oldest first. */
static GPtrArray *
waiting_for(Daemon *fixture, const gchar *agent_id)
{
	return clawt_mailbox_list(
		clawt_agent_get_mailbox(daemon_agent(fixture, agent_id)), NULL);
}

static JsonNode *
frame(Daemon *fixture, const gchar *kind, JsonNode *payload)
{
	g_autoptr(JsonNode) request = clawt_ipc_request_new(kind, "1");

	if (payload != NULL)
		clawt_ipc_frame_set_payload(request, payload);
	return clawt_daemon_handle_request(fixture->daemon, request);
}

/* chief delegates to beta; returns the task, which the daemon owns. */
static ClawtTask *
delegate(Daemon *fixture, const gchar *arguments)
{
	ClawtMcpTools *tools = clawt_daemon_get_mcp_tools(fixture->daemon);
	g_autoptr(JsonNode) response = call_tool_in(tools, "chief", NULL,
		"clawtilla_delegate", arguments);
	g_autoptr(GPtrArray) tasks = NULL;
	gboolean is_error = TRUE;

	response_text(response, &is_error);
	g_assert_false(is_error);

	tasks = clawt_task_manager_list(clawt_daemon_get_tasks(fixture->daemon),
		NULL, TRUE);
	g_assert_cmpuint(tasks->len, >, 0);
	return g_ptr_array_index(tasks, tasks->len - 1);
}

/*
 * beta takes the assignment waiting in its mailbox: leased the way the
 * drain takes it, and a turn begun on it the way the typing edge does.
 */
static ClawtMailboxItem *
beta_takes_its_work(Daemon *fixture)
{
	ClawtLinkServer *server = clawt_daemon_get_link_server(fixture->daemon);
	ClawtMailboxItem *item = clawt_mailbox_lease(
		clawt_agent_get_mailbox(daemon_agent(fixture, "beta")), 0);

	g_assert_nonnull(item);
	clawt_agent_deliver_item(daemon_agent(fixture, "beta"), item, TRUE);
	g_signal_emit_by_name(server, "typing", "beta",
		clawt_mailbox_item_get_room(item), TRUE);
	return item;
}

/*
 * `thread: new` through the daemon: the assignment lands in the task's
 * own room -- a session key of its own -- composed (the contract's own
 * lines survive the escape), and the task reports the room on the wire.
 * `mode: consult` marks that one delivery, and the turn it starts is a
 * consultation; a later note on the same task is not.
 */
static void
test_daemon_thread_and_consult_delegation(void)
{
	Daemon fixture = { 0 };
	ClawtTask *task;
	g_autoptr(GPtrArray) waiting = NULL;
	ClawtMailboxItem *item;
	g_autoptr(ClawtMailboxItem) taken = NULL;
	g_autofree gchar *shared = clawt_room_manager_direct_id("beta", "chief");
	g_autoptr(JsonNode) reply = NULL;
	JsonObject *shown;

	daemon_setup(&fixture);

	task = delegate(&fixture,
		"{\"agent_id\":\"beta\",\"task\":\"read the build log\","
		"\"thread\":\"new\",\"mode\":\"consult\"}");
	g_assert_nonnull(clawt_task_get_thread_room(task));
	g_assert_cmpstr(clawt_task_get_thread_room(task), !=, shared);

	waiting = waiting_for(&fixture, "beta");
	g_assert_cmpuint(waiting->len, ==, 1);
	item = g_ptr_array_index(waiting, 0);
	g_assert_cmpstr(clawt_mailbox_item_get_room(item), ==,
		clawt_task_get_thread_room(task));
	g_assert_cmpstr(clawt_mailbox_item_get_from(item), ==, "chief");
	g_assert_cmpstr(clawt_mailbox_item_get_task_id(item), ==,
		clawt_task_get_id(task));
	g_assert_true(clawt_mailbox_item_get_composed(item));
	g_assert_true(clawt_mailbox_item_get_consult(item));

	/* On the wire, for the clients. */
	{
		reply = frame(&fixture, "task.show", clawt_ipc_payload_new("task",
			clawt_task_get_id(task), NULL));
		g_assert_false(clawt_ipc_frame_is_error(reply));
		shown = json_object_get_object_member(
			clawt_ipc_frame_get_payload(reply), "task");
		g_assert_cmpstr(json_object_get_string_member(shown, "thread_room"),
			==, clawt_task_get_thread_room(task));
		g_assert_true(json_object_get_boolean_member(shown, "consult"));
	}

	/* The turn on it is a consultation, and its tools say so. */
	taken = beta_takes_its_work(&fixture);
	g_assert_true(clawt_mcp_tools_turn_is_consult(
		clawt_daemon_get_mcp_tools(fixture.daemon), "beta",
		clawt_task_get_thread_room(task)));

	/* The CLI's own Bash is refused through the permission hook too. */
	{
		g_autoptr(JsonNode) answer = NULL;
		JsonObject *answered;

		answer = frame(&fixture, "policy.check", clawt_ipc_payload_new(
			"agent", "beta", "tool", "Bash", "class", "exec",
			"room", clawt_task_get_thread_room(task), NULL));
		g_assert_false(clawt_ipc_frame_is_error(answer));
		answered = clawt_ipc_frame_get_payload(answer);
		g_assert_false(json_object_get_boolean_member(answered, "allowed"));
		g_assert_nonnull(strstr(json_object_get_string_member(answered,
			"reason"), "consultation"));
	}

	/* A later note on the same task is an ordinary message. */
	{
		ClawtMcpTools *tools = clawt_daemon_get_mcp_tools(fixture.daemon);
		g_autofree gchar *arguments = g_strdup_printf(
			"{\"agent_id\":\"beta\",\"body\":\"also check the tests\"}");
		g_autoptr(JsonNode) response = call_tool_in(tools, "chief", NULL,
			"clawtilla_message_agent", arguments);
		g_autoptr(GPtrArray) later = waiting_for(&fixture, "beta");
		gboolean is_error = TRUE;
		guint i;

		response_text(response, &is_error);
		g_assert_false(is_error);
		g_assert_cmpuint(later->len, >=, 1);
		for (i = 0; i < later->len; i++)
			g_assert_false(clawt_mailbox_item_get_consult(
				g_ptr_array_index(later, i)));
	}

	daemon_teardown(&fixture);
}

/*
 * A consultation's answer is marked in the room for the clients, and
 * the asker's own delivery of it is not a consultation.
 */
static void
test_daemon_consult_answer_is_marked(void)
{
	Daemon fixture = { 0 };
	ClawtMcpTools *tools;
	ClawtLinkServer *server;
	g_autoptr(ClawtMailboxItem) taken = NULL;
	g_autoptr(GPtrArray) waiting = NULL;
	g_autoptr(GPtrArray) history = NULL;
	g_autofree gchar *room = clawt_room_manager_direct_id("beta", "chief");
	gboolean is_error = TRUE;
	guint i;
	gboolean asked = FALSE;
	gboolean answered = FALSE;

	daemon_setup(&fixture);
	tools = clawt_daemon_get_mcp_tools(fixture.daemon);
	server = clawt_daemon_get_link_server(fixture.daemon);

	{
		g_autoptr(JsonNode) response = call_tool_in(tools, "chief", NULL,
			"clawtilla_ask_agent",
			"{\"agent_id\":\"beta\",\"message\":\"is the plan sound?\","
			"\"consult\":true}");

		response_text(response, &is_error);
		g_assert_false(is_error);
	}

	taken = beta_takes_its_work(&fixture);
	g_assert_true(clawt_mailbox_item_get_consult(taken));
	g_assert_cmpstr(clawt_mailbox_item_get_room(taken), ==, room);

	g_signal_emit_by_name(server, "typing", "beta", room, FALSE);
	g_signal_emit_by_name(server, "message", "beta", room,
		"It is sound, apart from the migration order.", NULL);

	history = clawt_room_get_history(clawt_room_manager_get(
		clawt_daemon_get_rooms(fixture.daemon), room), 0);
	for (i = 0; i < history->len; i++) {
		ClawtMessage *message = g_ptr_array_index(history, i);

		if (clawt_message_get_consult(message) == CLAWT_CONSULT_ASKED)
			asked = TRUE;
		if (clawt_message_get_consult(message) == CLAWT_CONSULT_ANSWERED)
			answered = TRUE;
	}
	g_assert_true(asked);
	g_assert_true(answered);

	waiting = waiting_for(&fixture, "chief");
	g_assert_cmpuint(waiting->len, ==, 1);
	g_assert_false(clawt_mailbox_item_get_consult(
		g_ptr_array_index(waiting, 0)));

	daemon_teardown(&fixture);
}

static ClawtDecision *
only_open_decision(Daemon *fixture)
{
	g_autoptr(GPtrArray) open = clawt_decision_store_list(
		clawt_daemon_get_decisions(fixture->daemon), TRUE);

	g_assert_cmpuint(open->len, ==, 1);
	return clawt_decision_copy(g_ptr_array_index(open, 0));
}

static gboolean
mail_mentions(Daemon *fixture, const gchar *agent_id, const gchar *needle)
{
	g_autoptr(GPtrArray) waiting = waiting_for(fixture, agent_id);
	guint i;

	for (i = 0; i < waiting->len; i++)
		if (strstr(clawt_mailbox_item_get_body(
				g_ptr_array_index(waiting, i)), needle) != NULL)
			return TRUE;
	return FALSE;
}

/*
 * A decision raised in a delegated turn: put to the delegator first in
 * the room it delegated from, shown to the operator with who it went
 * to, answerable by the delegator alone among agents, and the answer
 * reaches the asker attributed to the delegator.  Unanswered, the
 * default stands exactly as for any decision.
 */
static void
test_daemon_decision_routes_to_the_delegator(void)
{
	Daemon fixture = { 0 };
	ClawtMcpTools *tools;
	ClawtTask *task;
	g_autoptr(ClawtMailboxItem) taken = NULL;
	g_autoptr(ClawtDecision) decision = NULL;
	g_autoptr(ClawtDecision) settled = NULL;
	g_autofree gchar *arguments = NULL;
	gboolean is_error = TRUE;
	const gchar *text;

	daemon_setup(&fixture);
	tools = clawt_daemon_get_mcp_tools(fixture.daemon);

	/* Only a delegator is offered the answering tool. */
	{
		g_autoptr(GHashTable) chief_has = listed_names(tools, "chief", NULL);
		g_autoptr(GHashTable) beta_has = listed_names(tools, "beta", NULL);

		g_assert_true(g_hash_table_contains(chief_has,
			"clawtilla_decision_answer"));
		g_assert_false(g_hash_table_contains(beta_has,
			"clawtilla_decision_answer"));
	}

	task = delegate(&fixture,
		"{\"agent_id\":\"beta\",\"task\":\"pick a base image\"}");
	taken = beta_takes_its_work(&fixture);

	{
		g_autoptr(JsonNode) response = call_tool_in(tools, "beta", NULL,
			"clawtilla_ask_decision",
			"{\"question\":\"Fedora or Debian?\",\"options\":\"Fedora\\nDebian\","
			"\"default\":\"Fedora\",\"reversible_hours\":1}");

		text = response_text(response, &is_error);
		g_assert_false(is_error);
		g_assert_nonnull(strstr(text, "put to chief"));
	}

	decision = only_open_decision(&fixture);
	g_assert_cmpstr(clawt_decision_get_routed_to(decision), ==, "chief");
	g_assert_cmpstr(clawt_decision_get_task(decision), ==,
		clawt_task_get_id(task));
	g_assert_true(mail_mentions(&fixture, "chief",
		clawt_decision_get_id(decision)));
	g_assert_true(mail_mentions(&fixture, "chief",
		"clawtilla_decision_answer"));

	/* Nobody answers: the default stands once its time is up. */
	g_assert_true(clawt_decision_default_has_taken_effect(decision,
		g_get_real_time() / G_USEC_PER_SEC + 7200));

	/* The operator sees it, and who it went to. */
	{
		g_autoptr(JsonNode) reply = frame(&fixture, "decision.list", NULL);
		JsonArray *items = json_object_get_array_member(
			clawt_ipc_frame_get_payload(reply), "decisions");
		JsonObject *first = json_array_get_object_element(items, 0);
		g_autofree gchar *label = clawt_decision_routed_label(
			json_object_get_string_member(first, "routed_to"), NULL, TRUE);

		g_assert_cmpstr(json_object_get_string_member(first, "routed_to"),
			==, "chief");
		g_assert_nonnull(strstr(label, "chief"));
	}

	/* Another agent may not answer it. */
	arguments = g_strdup_printf(
		"{\"decision_id\":\"%s\",\"answer\":\"Debian\"}",
		clawt_decision_get_id(decision));
	{
		g_autoptr(JsonNode) response = NULL;

		/* gamma leads a team, so it is offered the tool... */
		response = call_tool_in(tools, "gamma", NULL,
			"clawtilla_decision_answer", arguments);
		text = response_text(response, &is_error);
		/* ...and refused this one, which is not its to answer. */
		g_assert_true(is_error);
		g_assert_nonnull(strstr(text, "not a decision put to you"));
	}

	/* The delegator answers; the asker is told, by name. */
	{
		g_autoptr(JsonNode) response = call_tool_in(tools, "chief", NULL,
			"clawtilla_decision_answer", arguments);

		text = response_text(response, &is_error);
		g_assert_false(is_error);
	}

	settled = clawt_decision_store_get(
		clawt_daemon_get_decisions(fixture.daemon),
		clawt_decision_get_id(decision));
	g_assert_cmpint(clawt_decision_get_state(settled), ==,
		CLAWT_DECISION_ANSWERED);
	g_assert_cmpstr(clawt_decision_get_answered_by(settled), ==, "chief");
	g_assert_cmpstr(clawt_decision_get_answer(settled), ==, "Debian");
	g_assert_true(mail_mentions(&fixture, "beta", "answered by 'chief'"));

	daemon_teardown(&fixture);
}

/*
 * The operator answering first wins, and the delegator is refused with
 * the reason; a decision raised outside delegated work goes to the
 * operator alone.
 */
static void
test_daemon_decision_operator_first_and_unrouted(void)
{
	Daemon fixture = { 0 };
	ClawtMcpTools *tools;
	g_autoptr(ClawtMailboxItem) taken = NULL;
	g_autoptr(ClawtDecision) decision = NULL;
	g_autofree gchar *arguments = NULL;
	gboolean is_error = FALSE;
	const gchar *text;

	daemon_setup(&fixture);
	tools = clawt_daemon_get_mcp_tools(fixture.daemon);

	/* Outside any delegated turn: the operator's alone. */
	{
		g_autoptr(JsonNode) response = call_tool_in(tools, "gamma", NULL,
			"clawtilla_ask_decision",
			"{\"question\":\"Rename it?\",\"default\":\"leave it\"}");
		g_autoptr(ClawtDecision) unrouted = NULL;

		response_text(response, &is_error);
		g_assert_false(is_error);
		unrouted = only_open_decision(&fixture);
		g_assert_null(clawt_decision_get_routed_to(unrouted));
		g_assert_null(clawt_decision_routed_label(NULL, NULL, TRUE));
		clawt_decision_store_dismiss(clawt_daemon_get_decisions(
			fixture.daemon), clawt_decision_get_id(unrouted), NULL);
	}

	delegate(&fixture, "{\"agent_id\":\"beta\",\"task\":\"ship it\"}");
	taken = beta_takes_its_work(&fixture);
	{
		g_autoptr(JsonNode) response = call_tool_in(tools, "beta", NULL,
			"clawtilla_ask_decision",
			"{\"question\":\"Today or Monday?\",\"default\":\"Monday\"}");

		response_text(response, &is_error);
		g_assert_false(is_error);
	}
	decision = only_open_decision(&fixture);
	g_assert_cmpstr(clawt_decision_get_routed_to(decision), ==, "chief");

	{
		g_autoptr(JsonNode) reply = NULL;

		reply = frame(&fixture, "decision.answer", clawt_ipc_payload_new(
			"decision", clawt_decision_get_id(decision),
			"answer", "Today", NULL));
		g_assert_false(clawt_ipc_frame_is_error(reply));
	}

	arguments = g_strdup_printf(
		"{\"decision_id\":\"%s\",\"answer\":\"Monday\"}",
		clawt_decision_get_id(decision));
	{
		g_autoptr(JsonNode) response = call_tool_in(tools, "chief", NULL,
			"clawtilla_decision_answer", arguments);

		text = response_text(response, &is_error);
		g_assert_true(is_error);
		g_assert_nonnull(strstr(text, "Already settled"));
	}

	daemon_teardown(&fixture);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/delegation-modes/consult-role-and-label",
		test_consult_role_and_label);
	g_test_add_func("/delegation-modes/consult-survives-and-migrates",
		test_consult_survives_a_restart_and_migrates);
	g_test_add_func("/delegation-modes/consult-withholds-acting-tools",
		test_consult_withholds_exactly_the_acting_tools);
	g_test_add_func("/delegation-modes/consult-before-turn-and-merged",
		test_consult_before_the_turn_and_when_merged);
	g_test_add_func("/delegation-modes/the-arguments", test_the_arguments);
	g_test_add_func("/delegation-modes/thread-new-room",
		test_thread_new_makes_and_reports_a_room);
	g_test_add_func("/delegation-modes/results-wait-for-busy-room",
		test_results_wait_for_the_busy_room);
	g_test_add_func("/delegation-modes/consultation-inside-a-round",
		test_consultation_inside_a_round);
	g_test_add_func("/delegation-modes/held-results-keep-their-round",
		test_held_results_keep_their_round);
	g_test_add_func("/delegation-modes/guard-idle-continuations",
		test_idle_continuations_stall);
	g_test_add_func("/delegation-modes/guard-same-error",
		test_same_error_three_times_stalls);
	g_test_add_func("/delegation-modes/guard-children",
		test_waiting_on_children_is_not_a_strike);
	g_test_add_func("/delegation-modes/turn-evidence", test_turn_evidence);
	g_test_add_func("/delegation-modes/daemon-thread-and-consult",
		test_daemon_thread_and_consult_delegation);
	g_test_add_func("/delegation-modes/daemon-consult-answer-marked",
		test_daemon_consult_answer_is_marked);
	g_test_add_func("/delegation-modes/daemon-decision-routed",
		test_daemon_decision_routes_to_the_delegator);
	g_test_add_func("/delegation-modes/daemon-decision-operator-first",
		test_daemon_decision_operator_first_and_unrouted);

	return g_test_run();
}
