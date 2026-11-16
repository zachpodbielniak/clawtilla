/*
 * test-replay.c - A turn a restart cut short: rerun once, or interrupted
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The mailbox keeps a record of every delivery until its turn ends; a
 * turn that reached outside the fleet is marked; after a restart an
 * unmarked turn is handed back once and every other one ends with an
 * interrupted notice in its room.  The daemon half includes
 * core/clawt-daemon-private.h, as test-shared-computer does.
 */

#include "clawtilla.h"
#include "clawt-test-util.h"
#include "core/clawt-daemon-private.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/socket.h>

/* ── Which calls are outward ─────────────────────────────────────── */

/* Whether @tool, called successfully, counts as outward. */
static gboolean
outward(
	const gchar       *tool,
	ClawtComputerType  computer
){
	g_autofree gchar *label = clawt_mcp_tools_outward_label(tool, computer);

	return label != NULL;
}

static gboolean
relay_outward(
	const gchar      *tool,
	ClawtActionClass  action
){
	g_autofree gchar *label = clawt_mcp_tools_relay_outward_label(tool,
		action);

	return label != NULL;
}

/*
 * One assertion per class, because the classification is a property of
 * the class table and a tool added later must land on the right side
 * without anybody remembering to list it.
 */
static void
test_outward_per_tool_class(void)
{
	/* A person, and the notifier behind it. */
	g_assert_true(outward("clawtilla_message_user", CLAWT_COMPUTER_NONE));

	/* Peers, tasks, rooms: inward, and rerunnable. */
	g_assert_false(outward("clawtilla_message_agent", CLAWT_COMPUTER_NONE));
	g_assert_false(outward("clawtilla_ask_agent", CLAWT_COMPUTER_NONE));
	g_assert_false(outward("clawtilla_delegate", CLAWT_COMPUTER_NONE));
	g_assert_false(outward("clawtilla_post_room", CLAWT_COMPUTER_NONE));
	g_assert_false(outward("clawtilla_task_complete", CLAWT_COMPUTER_NONE));

	/* clawtilla's own state, and every read. */
	g_assert_false(outward("clawtilla_memory_add", CLAWT_COMPUTER_NONE));
	g_assert_false(outward("clawtilla_routine_add", CLAWT_COMPUTER_NONE));
	g_assert_false(outward("clawtilla_create_agent", CLAWT_COMPUTER_NONE));
	g_assert_false(outward("clawtilla_web_fetch", CLAWT_COMPUTER_NONE));
	g_assert_false(outward("clawtilla_request_rebuild", CLAWT_COMPUTER_VM));

	/* A command: outward unless the machine is a sandbox. */
	g_assert_true(outward("clawtilla_computer_exec", CLAWT_COMPUTER_HOST));
	g_assert_true(outward("clawtilla_computer_exec", CLAWT_COMPUTER_DISTROBOX));
	g_assert_false(outward("clawtilla_computer_exec", CLAWT_COMPUTER_CONTAINER));
	g_assert_false(outward("clawtilla_computer_exec", CLAWT_COMPUTER_VM));

	/* External, desktop input and exec classes. */
	g_assert_true(outward("clawtilla_grok_message", CLAWT_COMPUTER_NONE));
	g_assert_true(outward("clawtilla_browser_click", CLAWT_COMPUTER_NONE));
	g_assert_true(outward("clawtilla_browser_eval", CLAWT_COMPUTER_NONE));
	g_assert_false(outward("clawtilla_browser_read", CLAWT_COMPUTER_NONE));

	/* A plugin's tool nobody classified is a third party's. */
	g_assert_true(outward("acme_send_invoice", CLAWT_COMPUTER_NONE));

	/* Through a relay: a connector writes unless its name reads. */
	g_assert_true(relay_outward("mcp__github__create_issue",
		CLAWT_ACTION_EXTERNAL));
	g_assert_true(relay_outward("send_email", CLAWT_ACTION_EXTERNAL));
	g_assert_false(relay_outward("mcp__github__list_issues",
		CLAWT_ACTION_EXTERNAL));
	g_assert_false(relay_outward("getFileContents", CLAWT_ACTION_EXTERNAL));
	g_assert_false(relay_outward("search-code", CLAWT_ACTION_EXTERNAL));
	g_assert_true(relay_outward("getaway_now", CLAWT_ACTION_EXTERNAL));
	g_assert_true(relay_outward("click", CLAWT_ACTION_DESKTOP_INPUT));

	/* The CLI's own tools are not judged: `ls` and `git push` look alike. */
	g_assert_false(relay_outward("Bash", CLAWT_ACTION_EXEC));
	g_assert_false(relay_outward("Read", CLAWT_ACTION_READ));
}

/* ── The mailbox's records ───────────────────────────────────────── */

/* Posts, leases and acknowledges one item: what a delivery leaves. */
static gchar *
delivered_item(
	ClawtMailbox *mailbox,
	const gchar  *body
){
	g_autoptr(ClawtMailboxItem) item = clawt_mailbox_item_new("user", "beta",
		body);
	g_autoptr(ClawtMailboxItem) leased = NULL;
	g_autoptr(GError) error = NULL;
	gchar *id;

	clawt_mailbox_item_set_room(item, "dm:beta:user");
	id = clawt_mailbox_post(mailbox, item, &error);
	g_assert_no_error(error);

	leased = clawt_mailbox_lease_id(mailbox, id, 0);
	g_assert_nonnull(leased);
	g_assert_true(clawt_mailbox_ack(mailbox, id, &error));
	g_assert_no_error(error);

	return id;
}

static gint64
open_for(
	ClawtMailbox *mailbox,
	const gchar  *id
){
	g_autoptr(ClawtMailboxItem) item = clawt_mailbox_get(mailbox, id);
	const gchar *ids[2] = { id, NULL };
	g_autoptr(GError) error = NULL;
	gint64 turn;

	turn = clawt_mailbox_turn_open(mailbox, "dm:beta:user", ids,
		clawt_mailbox_item_get_replays(item), &error);
	g_assert_no_error(error);
	g_assert_cmpint(turn, >, 0);

	return turn;
}

/*
 * A finished turn leaves nothing; an unmarked cut turn is handed back
 * once and a cut rerun is not handed back again, across as many
 * restarts as there are.
 */
static void
test_unmarked_is_rerun_once(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("clawt-replay-XXXXXX", NULL);
	g_autofree gchar *path = g_build_filename(dir, "mailbox.db", NULL);
	g_autoptr(ClawtMailbox) mailbox = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *done = NULL;
	g_autofree gchar *cut = NULL;
	gint64 turn;

	mailbox = clawt_mailbox_new("beta", path, &error);
	g_assert_no_error(error);

	/* A turn that ended normally is forgotten. */
	done = delivered_item(mailbox, "finished work");
	turn = open_for(mailbox, done);
	g_assert_cmpuint(clawt_mailbox_turn_count_open(mailbox), ==, 1);
	g_assert_true(clawt_mailbox_turn_close(mailbox, turn, &error));
	g_assert_cmpuint(clawt_mailbox_turn_count_open(mailbox), ==, 0);

	/* The first restart hands an unmarked turn back. */
	cut = delivered_item(mailbox, "write the report");
	open_for(mailbox, cut);
	{
		g_autoptr(GPtrArray) settled = clawt_mailbox_turn_recover(mailbox,
			TRUE, NULL, NULL, NULL, &error);
		ClawtTurnRecord *record;
		g_autoptr(ClawtMailboxItem) item = NULL;

		g_assert_no_error(error);
		g_assert_cmpuint(settled->len, ==, 1);
		record = g_ptr_array_index(settled, 0);
		g_assert_true(record->redelivered);
		g_assert_null(record->reason);
		g_assert_cmpstr(record->body, ==, "write the report");

		item = clawt_mailbox_get(mailbox, cut);
		g_assert_cmpint(clawt_mailbox_item_get_state(item), ==,
			CLAWT_MAILBOX_PENDING);
		g_assert_cmpuint(clawt_mailbox_item_get_replays(item), ==, 1);
		g_assert_cmpuint(clawt_mailbox_depth(mailbox), ==, 1);
	}

	/* The rerun is delivered, and cut again. */
	{
		g_autoptr(ClawtMailboxItem) leased = clawt_mailbox_lease(mailbox, 0);

		g_assert_nonnull(leased);
		g_assert_cmpstr(clawt_mailbox_item_get_id(leased), ==, cut);
		g_assert_cmpuint(clawt_mailbox_item_get_replays(leased), ==, 1);
		g_assert_true(clawt_mailbox_ack(mailbox, cut, &error));
		open_for(mailbox, cut);
	}

	/* The second restart does not hand it back a second time. */
	{
		g_autoptr(GPtrArray) settled = clawt_mailbox_turn_recover(mailbox,
			TRUE, NULL, NULL, NULL, &error);
		ClawtTurnRecord *record;
		g_autoptr(ClawtMailboxItem) item = NULL;

		g_assert_no_error(error);
		g_assert_cmpuint(settled->len, ==, 1);
		record = g_ptr_array_index(settled, 0);
		g_assert_false(record->redelivered);
		g_assert_nonnull(strstr(record->reason, "already handed back"));

		item = clawt_mailbox_get(mailbox, cut);
		g_assert_cmpint(clawt_mailbox_item_get_state(item), ==,
			CLAWT_MAILBOX_ACKED);
		g_assert_cmpuint(clawt_mailbox_depth(mailbox), ==, 0);
	}

	/* And a third finds nothing left to decide. */
	{
		g_autoptr(GPtrArray) settled = clawt_mailbox_turn_recover(mailbox,
			TRUE, NULL, NULL, NULL, &error);

		g_assert_no_error(error);
		g_assert_cmpuint(settled->len, ==, 0);
		g_assert_cmpuint(clawt_mailbox_depth(mailbox), ==, 0);
	}

	g_clear_object(&mailbox);
	clawt_test_remove_tree(dir);
}

/* A turn that acted outward is never rerun, and says what it did. */
static void
test_marked_becomes_interrupted(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("clawt-replay-XXXXXX", NULL);
	g_autofree gchar *path = g_build_filename(dir, "mailbox.db", NULL);
	g_autoptr(ClawtMailbox) mailbox = NULL;
	g_autoptr(GPtrArray) settled = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *id = NULL;
	ClawtTurnRecord *record;
	gint64 turn;

	mailbox = clawt_mailbox_new("beta", path, &error);
	g_assert_no_error(error);

	id = delivered_item(mailbox, "email the customer");
	turn = open_for(mailbox, id);

	/* Each label once, however often the turn does the thing. */
	g_assert_true(clawt_mailbox_turn_mark_outward(mailbox, turn,
		"sent the operator a message", &error));
	g_assert_true(clawt_mailbox_turn_mark_outward(mailbox, turn,
		"used send_email through a connector", &error));
	g_assert_true(clawt_mailbox_turn_mark_outward(mailbox, turn,
		"sent the operator a message", &error));
	g_assert_no_error(error);

	settled = clawt_mailbox_turn_recover(mailbox, TRUE, NULL, NULL, NULL,
		&error);
	g_assert_no_error(error);
	g_assert_cmpuint(settled->len, ==, 1);
	record = g_ptr_array_index(settled, 0);
	g_assert_false(record->redelivered);
	g_assert_cmpstr(record->outward, ==, "sent the operator a message, "
		"used send_email through a connector");
	g_assert_nonnull(strstr(record->reason, "outside the fleet"));
	g_assert_cmpuint(clawt_mailbox_depth(mailbox), ==, 0);
	g_assert_cmpuint(clawt_mailbox_turn_count_open(mailbox), ==, 0);

	g_clear_object(&mailbox);
	clawt_test_remove_tree(dir);
}

/* A caller's veto ends a turn that would have been rerun. */
static const gchar *
veto_everything(
	ClawtMailbox    *mailbox,
	ClawtTurnRecord *record,
	gpointer         user_data
){
	(void)mailbox;
	(void)record;

	*(guint *)user_data += 1;
	return "the task it was working on had already ended";
}

static void
test_veto_stops_a_rerun(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("clawt-replay-XXXXXX", NULL);
	g_autofree gchar *path = g_build_filename(dir, "mailbox.db", NULL);
	g_autoptr(ClawtMailbox) mailbox = NULL;
	g_autoptr(GPtrArray) settled = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *id = NULL;
	guint asked = 0;

	mailbox = clawt_mailbox_new("beta", path, &error);
	g_assert_no_error(error);
	id = delivered_item(mailbox, "work for a failed task");
	open_for(mailbox, id);

	settled = clawt_mailbox_turn_recover(mailbox, TRUE, NULL,
		veto_everything, &asked, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(asked, ==, 1);
	g_assert_false(((ClawtTurnRecord *)g_ptr_array_index(settled, 0))->redelivered);
	g_assert_cmpuint(clawt_mailbox_depth(mailbox), ==, 0);

	g_clear_object(&mailbox);
	clawt_test_remove_tree(dir);
}

/*
 * The records are on disk: a mailbox reopened by the next daemon finds
 * the turn the last one left open, and the acknowledged item it names
 * survives the sweep that would otherwise reap it.
 */
static void
test_records_survive_reopening(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("clawt-replay-XXXXXX", NULL);
	g_autofree gchar *path = g_build_filename(dir, "mailbox.db", NULL);
	g_autofree gchar *id = NULL;
	g_autoptr(GError) error = NULL;

	{
		g_autoptr(ClawtMailbox) mailbox = clawt_mailbox_new("beta", path,
			&error);

		g_assert_no_error(error);
		id = delivered_item(mailbox, "long job");
		open_for(mailbox, id);
	}

	{
		g_autoptr(ClawtMailbox) mailbox = clawt_mailbox_new("beta", path,
			&error);
		g_autoptr(GPtrArray) open = NULL;

		g_assert_no_error(error);
		g_assert_cmpuint(clawt_mailbox_turn_count_open(mailbox), ==, 1);
		open = clawt_mailbox_turn_list_open(mailbox);
		g_assert_cmpstr(((ClawtTurnRecord *)g_ptr_array_index(open, 0))->items[0],
			==, id);
		g_assert_cmpstr(((ClawtTurnRecord *)g_ptr_array_index(open, 0))->room,
			==, "dm:beta:user");
	}

	clawt_test_remove_tree(dir);
}

/* ── The daemon ──────────────────────────────────────────────────── */

typedef struct {
	gchar        *dir;
	GMainContext *context;
	ClawtDaemon  *daemon;
	GSocket      *far_end;
} DaemonFixture;

static const gchar FLEET[] =
	"agents:\n"
	"  - id: beta\n    computer:\n      type: none\n"
	"    runtime:\n      autostart: false\n"
	"  - id: alpha\n    computer:\n      type: none\n"
	"    runtime:\n      autostart: false\n";

static void
daemon_setup(
	DaemonFixture *fixture
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *yaml = NULL;

	fixture->dir = g_dir_make_tmp("clawt-replay-XXXXXX", NULL);
	path = g_build_filename(fixture->dir, "config.yaml", NULL);
	yaml = g_strdup_printf("daemon:\n  tailscale: false\n  state_dir: \"%s/state\"\n"
		"  socket: \"%s/d.sock\"\n  automation_dir: \"%s/pods\"\n"
		"defaults:\n  workspace_root: \"%s/agents\"\n%s",
		fixture->dir, fixture->dir, fixture->dir, fixture->dir, FLEET);
	g_assert_true(g_file_set_contents(path, yaml, -1, &error));
	fixture->context = g_main_context_new();
	fixture->daemon = clawt_daemon_new(path, fixture->context);
	g_assert_true(clawt_daemon_start(fixture->daemon, NULL));
}

static void
daemon_teardown(
	DaemonFixture *fixture
){
	clawt_daemon_stop(fixture->daemon);
	g_clear_object(&fixture->daemon);
	while (g_main_context_iteration(fixture->context, FALSE))
		;
	g_clear_pointer(&fixture->context, g_main_context_unref);
	g_clear_object(&fixture->far_end);
	clawt_test_remove_tree(fixture->dir);
	g_free(fixture->dir);
}

/* A socketpair standing in for the agent's process; the drain writes it. */
static void
give_link(
	DaemonFixture *fixture,
	const gchar   *agent_id
){
	ClawtAgent *agent = clawt_agent_manager_get(fixture->daemon->agents,
		agent_id);
	g_autoptr(GSocket) near_end = NULL;
	g_autoptr(GSocketConnection) connection = NULL;
	g_autoptr(ClawtLink) link = NULL;
	g_autoptr(GError) error = NULL;
	int fds[2];

	g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), ==, 0);
	near_end = g_socket_new_from_fd(fds[0], &error);
	g_assert_no_error(error);
	g_clear_object(&fixture->far_end);
	fixture->far_end = g_socket_new_from_fd(fds[1], &error);
	g_assert_no_error(error);

	connection = g_socket_connection_factory_create_connection(near_end);
	link = clawt_link_new(connection);
	clawt_agent_set_link(agent, link);
}

/* What the agent has been handed since the last read. */
static gchar *
read_all(
	DaemonFixture *fixture
){
	g_autoptr(GString) out = g_string_new(NULL);
	gchar buffer[16384];

	g_socket_set_blocking(fixture->far_end, FALSE);
	for (;;) {
		gssize got = g_socket_receive(fixture->far_end, buffer,
			sizeof(buffer), NULL, NULL);

		if (got <= 0)
			break;
		g_string_append_len(out, buffer, got);
	}

	return g_string_free(g_steal_pointer(&out), FALSE);
}

/* The process died: what the agent's runtime exit leaves behind. */
static void
kill_process(
	DaemonFixture *fixture,
	const gchar   *agent_id
){
	ClawtAgent *agent = clawt_agent_manager_get(fixture->daemon->agents,
		agent_id);

	clawt_agent_set_link(agent, NULL);
	clawt_agent_clear_typing(agent);
}

/* The newest message in @room_id that is an interrupted notice. */
static ClawtMessage *
last_interrupted(
	DaemonFixture *fixture,
	const gchar   *room_id
){
	ClawtRoom *room = clawt_room_manager_get(fixture->daemon->rooms, room_id);
	g_autoptr(GPtrArray) history = NULL;
	guint i;

	if (room == NULL)
		return NULL;

	history = clawt_room_get_history(room, 0);
	for (i = history->len; i > 0; i--) {
		ClawtMessage *message = g_ptr_array_index(history, i - 1);

		if (clawt_message_get_interrupted(message))
			return clawt_message_copy(message);
	}

	return NULL;
}

static guint
count_interrupted(
	DaemonFixture *fixture,
	const gchar   *room_id
){
	ClawtRoom *room = clawt_room_manager_get(fixture->daemon->rooms, room_id);
	g_autoptr(GPtrArray) history = NULL;
	guint count = 0;
	guint i;

	if (room == NULL)
		return 0;

	history = clawt_room_get_history(room, 0);
	for (i = 0; i < history->len; i++)
		if (clawt_message_get_interrupted(g_ptr_array_index(history, i)))
			count++;

	return count;
}

/* A clawtilla tool call, as the agent's MCP link would make it. */
static gboolean
call_tool(
	DaemonFixture *fixture,
	const gchar   *agent_id,
	const gchar   *room_id,
	const gchar   *tool,
	const gchar   *arguments
){
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(JsonNode) response = NULL;
	g_autofree gchar *request = g_strdup_printf(
		"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
		"\"params\":{\"name\":\"%s\",\"arguments\":%s}}", tool, arguments);
	JsonObject *result;

	g_assert_true(json_parser_load_from_data(parser, request, -1, NULL));
	response = clawt_mcp_tools_call(fixture->daemon->mcp_tools, agent_id,
		room_id, json_parser_get_root(parser));
	result = json_object_get_object_member(json_node_get_object(response),
		"result");

	return !json_object_get_boolean_member_with_default(result, "isError",
		FALSE);
}

/*
 * The whole path, with two rooms so a mark cannot leak from one turn to
 * the other: the operator's turn sends them a message and is marked; a
 * peer's turn does nothing outward.  The first restart reruns the peer's
 * and posts an interrupted notice for the operator's; the rerun, cut
 * again, ends as interrupted; a third restart does nothing at all.
 */
static void
test_daemon_restart_reruns_once_and_notices_the_rest(void)
{
	DaemonFixture fixture = { 0 };
	ClawtAgent *beta;
	ClawtMailbox *mailbox;
	g_autofree gchar *user_room = clawt_room_manager_direct_id("user", "beta");
	g_autofree gchar *peer_room = clawt_room_manager_direct_id("alpha", "beta");
	g_autofree gchar *handed = NULL;
	g_autoptr(GError) error = NULL;

	daemon_setup(&fixture);
	beta = clawt_agent_manager_get(fixture.daemon->agents, "beta");
	mailbox = clawt_agent_get_mailbox(beta);
	g_assert_nonnull(mailbox);
	give_link(&fixture, "beta");

	/* Two deliveries, two rooms, two turns running at once. */
	g_assert_cmpint(clawt_mailbox_router_send_to(fixture.daemon->router,
		"user", "beta", "tell me when the build is green", NULL, 0, &error),
		>=, 0);
	g_assert_cmpint(clawt_mailbox_router_send_to(fixture.daemon->router,
		"alpha", "beta", "please tidy the notes", NULL, 1, &error), >=, 0);
	g_assert_no_error(error);
	g_assert_cmpuint(clawt_mailbox_turn_count_open(mailbox), ==, 2);

	clawt_agent_note_typing(beta, user_room, TRUE);
	clawt_agent_note_typing(beta, peer_room, TRUE);

	/* The operator's turn reaches the operator; the mark is per room. */
	g_assert_true(call_tool(&fixture, "beta", user_room,
		"clawtilla_message_user", "{\"body\":\"the build is green\"}"));
	/* A peer message is inward and marks nothing. */
	(void)call_tool(&fixture, "beta", peer_room, "clawtilla_message_agent",
		"{\"agent_id\":\"alpha\",\"body\":\"on it\"}");

	{
		g_autoptr(GPtrArray) open = clawt_mailbox_turn_list_open(mailbox);
		guint i;
		guint marked = 0;

		for (i = 0; i < open->len; i++) {
			ClawtTurnRecord *record = g_ptr_array_index(open, i);

			if (record->outward != NULL) {
				marked++;
				g_assert_cmpstr(record->room, ==, user_room);
			}
		}
		g_assert_cmpuint(marked, ==, 1);
	}

	/* The process dies; the daemon settles what it was running. */
	kill_process(&fixture, "beta");
	g_assert_cmpuint(clawt_daemon_turns_recover(fixture.daemon, "beta"),
		==, 1);

	{
		g_autoptr(ClawtMessage) notice = last_interrupted(&fixture, user_room);

		g_assert_nonnull(notice);
		g_assert_cmpstr(clawt_message_get_sender_id(notice), ==,
			CLAWT_SYSTEM_SENDER);
		g_assert_false(clawt_message_get_invites_reply(notice));
		g_assert_nonnull(strstr(clawt_message_get_body(notice),
			"sent the operator a message"));
		g_assert_nonnull(strstr(clawt_message_get_body(notice),
			"tell me when the build is green"));
	}
	g_assert_cmpuint(count_interrupted(&fixture, peer_room), ==, 0);
	g_assert_cmpuint(clawt_mailbox_depth(mailbox), ==, 1);

	/* The agent comes back and is handed the peer's message, as a rerun. */
	g_free(read_all(&fixture));
	give_link(&fixture, "beta");
	clawt_mailbox_router_drain(fixture.daemon->router, "beta");
	handed = read_all(&fixture);
	g_assert_nonnull(strstr(handed, "please tidy the notes"));
	g_assert_nonnull(strstr(handed, "handed to you a second time"));
	g_assert_cmpuint(clawt_mailbox_turn_count_open(mailbox), ==, 1);

	/* Cut again: interrupted, not handed back a third time. */
	clawt_agent_note_typing(beta, peer_room, TRUE);
	kill_process(&fixture, "beta");
	g_assert_cmpuint(clawt_daemon_turns_recover(fixture.daemon, "beta"),
		==, 0);
	g_assert_cmpuint(count_interrupted(&fixture, peer_room), ==, 1);
	{
		g_autoptr(ClawtMessage) notice = last_interrupted(&fixture, peer_room);

		g_assert_nonnull(strstr(clawt_message_get_body(notice),
			"already handed back once"));
	}
	g_assert_cmpuint(clawt_mailbox_depth(mailbox), ==, 0);

	/* Nothing is left to settle, so a third restart changes nothing. */
	g_assert_cmpuint(clawt_daemon_turns_recover(fixture.daemon, "beta"),
		==, 0);
	g_assert_cmpuint(count_interrupted(&fixture, peer_room), ==, 1);
	g_assert_cmpuint(count_interrupted(&fixture, user_room), ==, 1);

	daemon_teardown(&fixture);
}

/*
 * A turn that ends -- on its own falling edge, or because the daemon
 * ended it (an interrupt, the grace timer) -- leaves no record, so a
 * later restart reruns nothing that finished.
 */
static void
test_ended_turns_leave_nothing(void)
{
	DaemonFixture fixture = { 0 };
	ClawtAgent *beta;
	ClawtMailbox *mailbox;
	g_autofree gchar *user_room = clawt_room_manager_direct_id("user", "beta");
	g_autoptr(GError) error = NULL;

	daemon_setup(&fixture);
	beta = clawt_agent_manager_get(fixture.daemon->agents, "beta");
	mailbox = clawt_agent_get_mailbox(beta);
	give_link(&fixture, "beta");

	clawt_mailbox_router_send_to(fixture.daemon->router, "user", "beta",
		"first", NULL, 0, &error);
	clawt_agent_note_typing(beta, user_room, TRUE);
	clawt_agent_note_typing(beta, user_room, FALSE);
	g_assert_cmpuint(clawt_mailbox_turn_count_open(mailbox), ==, 0);

	/* An interrupt settles the turn through the daemon. */
	clawt_mailbox_router_send_to(fixture.daemon->router, "user", "beta",
		"second", NULL, 0, &error);
	clawt_agent_note_typing(beta, user_room, TRUE);
	g_assert_cmpuint(clawt_mailbox_turn_count_open(mailbox), ==, 1);
	clawt_daemon_turn_settle(fixture.daemon, "beta");
	g_assert_cmpuint(clawt_mailbox_turn_count_open(mailbox), ==, 0);

	kill_process(&fixture, "beta");
	g_assert_cmpuint(clawt_daemon_turns_recover(fixture.daemon, "beta"),
		==, 0);
	g_assert_cmpuint(count_interrupted(&fixture, user_room), ==, 0);

	daemon_teardown(&fixture);
}

/* A stop somebody asked for ends the turn as interrupted, with no rerun. */
static void
test_operator_stop_is_not_rerun(void)
{
	DaemonFixture fixture = { 0 };
	ClawtAgent *beta;
	ClawtMailbox *mailbox;
	g_autofree gchar *user_room = clawt_room_manager_direct_id("user", "beta");
	g_autoptr(ClawtMessage) notice = NULL;
	g_autoptr(GError) error = NULL;

	daemon_setup(&fixture);
	beta = clawt_agent_manager_get(fixture.daemon->agents, "beta");
	mailbox = clawt_agent_get_mailbox(beta);
	give_link(&fixture, "beta");

	clawt_mailbox_router_send_to(fixture.daemon->router, "user", "beta",
		"refactor everything", NULL, 0, &error);
	clawt_agent_note_typing(beta, user_room, TRUE);

	clawt_daemon_turns_abandon(fixture.daemon, "beta",
		"its operator stopped it");
	kill_process(&fixture, "beta");
	g_assert_cmpuint(clawt_daemon_turns_recover(fixture.daemon, "beta"),
		==, 0);

	notice = last_interrupted(&fixture, user_room);
	g_assert_nonnull(notice);
	g_assert_nonnull(strstr(clawt_message_get_body(notice),
		"its operator stopped it"));
	g_assert_cmpuint(clawt_mailbox_depth(mailbox), ==, 0);

	daemon_teardown(&fixture);
}

/*
 * The notice's mark survives the transcript, and room.history carries
 * it for a client opening the conversation later.
 */
static void
test_notice_is_marked_on_the_wire(void)
{
	g_autoptr(ClawtMessage) message = clawt_message_new("room",
		CLAWT_SYSTEM_SENDER, "[clawtilla] cut short");
	g_autoptr(ClawtMessage) copy = NULL;

	g_assert_false(clawt_message_get_interrupted(message));
	clawt_message_set_interrupted(message, TRUE);
	copy = clawt_message_copy(message);
	g_assert_true(clawt_message_get_interrupted(copy));
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);

	g_test_add_func("/replay/outward-per-tool-class",
		test_outward_per_tool_class);
	g_test_add_func("/replay/unmarked-is-rerun-once",
		test_unmarked_is_rerun_once);
	g_test_add_func("/replay/marked-becomes-interrupted",
		test_marked_becomes_interrupted);
	g_test_add_func("/replay/veto-stops-a-rerun", test_veto_stops_a_rerun);
	g_test_add_func("/replay/records-survive-reopening",
		test_records_survive_reopening);
	g_test_add_func("/replay/daemon-reruns-once-and-notices-the-rest",
		test_daemon_restart_reruns_once_and_notices_the_rest);
	g_test_add_func("/replay/ended-turns-leave-nothing",
		test_ended_turns_leave_nothing);
	g_test_add_func("/replay/operator-stop-is-not-rerun",
		test_operator_stop_is_not_rerun);
	g_test_add_func("/replay/notice-is-marked",
		test_notice_is_marked_on_the_wire);

	return g_test_run();
}
