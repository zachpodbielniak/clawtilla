/*
 * test-steering.c - Which deliveries steer, and the turn state they carry
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Three layers, each on its own and then together:
 *
 *   - the policy (clawt_steer_decide()): every level, and every rule no
 *     level lifts -- another room's turn, a group room, a held result, a
 *     daemon notice, another task, a model of its own, the consultation
 *     line;
 *   - the turn state: a steered delivery's description folds into the
 *     running turn's record in the safe direction and is never popped by
 *     the next real turn (the bug class on_runtime_exited() hit), a
 *     refused one waits for its own turn in libreclaw's queue order, and
 *     the mailbox turn record joins the running turn's;
 *   - the router: what goes on the wire, over a real socketpair.
 *
 * And the link (hello features, the request, chat.steered) and the room
 * mark that lets every client draw a steered message as steered.
 */

#include <clawtilla.h>

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>

#include <string.h>
#include <sys/socket.h>

#include "clawt-test-util.h"

/* ── The policy ──────────────────────────────────────────────────── */

/* A delivery from the delegator into its own running turn. */
static ClawtSteerFacts
facts_base(void)
{
	ClawtSteerFacts facts;

	memset(&facts, 0, sizeof(facts));
	facts.policy = CLAWT_STEER_DELEGATOR;
	facts.link_can_steer = TRUE;
	facts.turn_running = TRUE;
	facts.sender = "chief";
	facts.sender_is_agent = TRUE;
	facts.turn_origin = "chief";
	return facts;
}

static void
test_policy_levels(void)
{
	ClawtSteerFacts facts = facts_base();
	const gchar *why = NULL;

	/* The delegator's correction steers under the default. */
	g_assert_true(clawt_steer_decide(&facts, &why));
	g_assert_nonnull(why);

	/* never means never. */
	facts.policy = CLAWT_STEER_NEVER;
	g_assert_false(clawt_steer_decide(&facts, NULL));

	/* The delegator of the turn's task counts as handing out the work. */
	facts = facts_base();
	facts.turn_origin = CLAWT_SYSTEM_SENDER;
	facts.turn_task_id = "task-1";
	facts.turn_task_delegator = "chief";
	g_assert_true(clawt_steer_decide(&facts, NULL));

	/* Another agent: only from peers up. */
	facts = facts_base();
	facts.sender = "kudu";
	g_assert_false(clawt_steer_decide(&facts, &why));
	g_assert_cmpstr(why, ==, "its sender did not hand out the running "
		"turn's work");
	facts.policy = CLAWT_STEER_PEERS;
	g_assert_true(clawt_steer_decide(&facts, NULL));
	facts.policy = CLAWT_STEER_ALL;
	g_assert_true(clawt_steer_decide(&facts, NULL));

	/* A person: only under all, and only in their own room. */
	facts = facts_base();
	facts.sender = "user";
	facts.sender_is_agent = FALSE;
	facts.person_room = TRUE;
	g_assert_false(clawt_steer_decide(&facts, NULL));
	facts.policy = CLAWT_STEER_PEERS;
	g_assert_false(clawt_steer_decide(&facts, NULL));
	facts.policy = CLAWT_STEER_ALL;
	g_assert_true(clawt_steer_decide(&facts, NULL));
	facts.person_room = FALSE;
	g_assert_false(clawt_steer_decide(&facts, NULL));

	/* Somebody who is neither: a webhook, a bot. */
	facts = facts_base();
	facts.policy = CLAWT_STEER_ALL;
	facts.sender = "webhook:ci";
	facts.sender_is_agent = FALSE;
	g_assert_false(clawt_steer_decide(&facts, NULL));
}

/* The rules no level lifts, each under the most permissive level. */
static void
test_policy_hard_rules(void)
{
	ClawtSteerFacts facts;
	const gchar *why = NULL;

#define ALL_BUT(field, value) \
	facts = facts_base(); facts.policy = CLAWT_STEER_ALL; facts.field = value

	ALL_BUT(link_can_steer, FALSE);
	g_assert_false(clawt_steer_decide(&facts, &why));
	g_assert_cmpstr(why, ==, "the agent does not answer steer requests");

	/* Never into another room's turn: nothing runs in this one. */
	ALL_BUT(turn_running, FALSE);
	g_assert_false(clawt_steer_decide(&facts, &why));
	g_assert_cmpstr(why, ==, "no turn is running in its room");

	/* Rounds and threads decide a group room's turns. */
	ALL_BUT(group_room, TRUE);
	g_assert_false(clawt_steer_decide(&facts, NULL));

	/* Held results stay held, notices take a turn. */
	ALL_BUT(is_result, TRUE);
	g_assert_false(clawt_steer_decide(&facts, &why));
	g_assert_cmpstr(why, ==, "a result waits for the turn to end");
	ALL_BUT(sender, CLAWT_SYSTEM_SENDER);
	g_assert_false(clawt_steer_decide(&facts, NULL));

	/* A model of its own, another task. */
	ALL_BUT(model, "haiku");
	g_assert_false(clawt_steer_decide(&facts, NULL));
	ALL_BUT(task_id, "task-2");
	facts.turn_task_id = "task-1";
	g_assert_false(clawt_steer_decide(&facts, &why));
	g_assert_cmpstr(why, ==, "it is about another task");

	/* The same task, or none, is not another one. */
	ALL_BUT(task_id, "task-1");
	facts.turn_task_id = "task-1";
	g_assert_true(clawt_steer_decide(&facts, NULL));

	/* The consultation line, both ways. */
	ALL_BUT(consult, TRUE);
	g_assert_false(clawt_steer_decide(&facts, &why));
	g_assert_cmpstr(why, ==,
		"a consultation never steers into a turn that can act");
	ALL_BUT(turn_consult, TRUE);
	g_assert_false(clawt_steer_decide(&facts, &why));
	g_assert_cmpstr(why, ==,
		"an ordinary message never steers into a consultation");

	/* Into a consultation: the consulting agent asking again, only. */
	ALL_BUT(consult, TRUE);
	facts.turn_consult = TRUE;
	g_assert_true(clawt_steer_decide(&facts, NULL));
	facts.sender = "kudu";
	g_assert_false(clawt_steer_decide(&facts, &why));
	g_assert_cmpstr(why, ==,
		"only the consulting agent steers into a consultation");

#undef ALL_BUT
}

static void
test_policy_vocabulary(void)
{
	guint i;

	g_assert_cmpuint(clawt_steer_policy_count(), ==, 4);
	g_assert_cmpint(clawt_steer_policy_from_nick(NULL), ==,
		CLAWT_STEER_DELEGATOR);
	g_assert_cmpint(clawt_steer_policy_from_nick("nonsense"), ==,
		CLAWT_STEER_DELEGATOR);

	/* The table and the GType agree on every spelling. */
	for (i = 0; i < clawt_steer_policy_count(); i++) {
		const gchar *nick = clawt_steer_policy_nick((ClawtSteerPolicy)i);
		gint value = -1;

		g_assert_true(clawt_enum_from_nick(CLAWT_TYPE_STEER_POLICY, nick,
			&value));
		g_assert_cmpint(value, ==, (gint)i);
		g_assert_cmpint(clawt_steer_policy_from_nick(nick), ==, (gint)i);
		g_assert_nonnull(clawt_steer_policy_label((ClawtSteerPolicy)i));
		g_assert_cmpuint(strlen(clawt_steer_policy_explanation(
			(ClawtSteerPolicy)i)), >, 40);
	}

	g_assert_true(clawt_steer_outcome_is_steered("delivered"));
	g_assert_true(clawt_steer_outcome_is_steered("indeterminate"));
	g_assert_false(clawt_steer_outcome_is_steered("refused"));
	g_assert_false(clawt_steer_outcome_is_steered(NULL));
	g_assert_nonnull(strstr(clawt_message_steered_tooltip("indeterminate"),
		"not sent again"));
}

/* ── The router, over a real socketpair ──────────────────────────── */

typedef struct {
	gchar              *dir;
	ClawtConfig        *config;
	ClawtAgentManager  *agents;
	ClawtRoomManager   *rooms;
	ClawtMailboxRouter *router;
	GSocket            *far_end;
} Fixture;

static void
fixture_setup(Fixture *fixture, const gchar *agents_yaml)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *yaml = NULL;

	fixture->dir = g_dir_make_tmp("clawt-steer-XXXXXX", NULL);

	/* All three pinned, or a fixture scaffolds into the real fleet. */
	yaml = g_strdup_printf("daemon:\n"
	                       "  state_dir: \"%s\"\n"
	                       "  socket: \"%s/daemon.sock\"\n"
	                       "defaults:\n  workspace_root: \"%s/agents\"\n%s",
	                       fixture->dir, fixture->dir, fixture->dir,
	                       agents_yaml);

	fixture->config = clawt_config_load_from_string(yaml, &error);
	g_assert_no_error(error);

	fixture->agents = clawt_agent_manager_new(fixture->config);
	clawt_agent_manager_load(fixture->agents, NULL);

	fixture->rooms = clawt_room_manager_new(NULL);
	fixture->router = clawt_mailbox_router_new(fixture->agents,
	                                           fixture->rooms, NULL);
}

static void
fixture_teardown(Fixture *fixture)
{
	g_clear_object(&fixture->far_end);
	g_clear_object(&fixture->router);
	g_clear_object(&fixture->rooms);
	g_clear_object(&fixture->agents);
	g_clear_object(&fixture->config);

	if (fixture->dir != NULL)
		clawt_test_remove_tree(fixture->dir);

	g_clear_pointer(&fixture->dir, g_free);
}

/* A link the drain really writes into, which says it steers or not. */
static ClawtLink *
give_beta_a_link(
	Fixture  *fixture,
	gboolean  can_steer
){
	ClawtAgent *agent = clawt_agent_manager_get(fixture->agents, "beta");
	g_autoptr(GSocket) near_end = NULL;
	g_autoptr(GSocketConnection) connection = NULL;
	g_autoptr(ClawtLink) link = NULL;
	g_autoptr(GError) error = NULL;
	int fds[2];

	g_assert_nonnull(agent);
	g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), ==, 0);

	near_end = g_socket_new_from_fd(fds[0], &error);
	g_assert_no_error(error);
	fixture->far_end = g_socket_new_from_fd(fds[1], &error);
	g_assert_no_error(error);

	connection = g_socket_connection_factory_create_connection(near_end);
	link = clawt_link_new(connection);
	clawt_link_set_can_steer(link, can_steer);
	clawt_agent_set_link(agent, link);

	return link;
}

/* The payloads beta has been handed since the last call, in order. */
static GPtrArray *
read_payloads(Fixture *fixture)
{
	g_autoptr(GString) out = g_string_new(NULL);
	g_auto(GStrv) lines = NULL;
	GPtrArray *payloads;
	gchar buffer[16384];
	guint i;

	g_socket_set_blocking(fixture->far_end, FALSE);

	for (;;) {
		gssize got = g_socket_receive(fixture->far_end, buffer,
			sizeof(buffer), NULL, NULL);

		if (got <= 0)
			break;
		g_string_append_len(out, buffer, got);
	}

	payloads = g_ptr_array_new_with_free_func(
		(GDestroyNotify)json_object_unref);
	lines = g_strsplit(out->str, "\n", -1);

	for (i = 0; lines[i] != NULL; i++) {
		g_autoptr(JsonParser) parser = json_parser_new();
		JsonObject *envelope;

		if (lines[i][0] == '\0' ||
			!json_parser_load_from_data(parser, lines[i], -1, NULL))
			continue;

		envelope = json_node_get_object(json_parser_get_root(parser));
		g_ptr_array_add(payloads, json_object_ref(
			json_object_get_object_member(envelope, "payload")));
	}

	return payloads;
}

/* The one payload beta was handed since the last call. */
static JsonObject *
read_one(Fixture *fixture)
{
	g_autoptr(GPtrArray) payloads = read_payloads(fixture);

	g_assert_cmpuint(payloads->len, ==, 1);
	return json_object_ref(g_ptr_array_index(payloads, 0));
}

/* What alpha sends beta: a message at @depth, inviting a reply or not. */
static void
alpha_says(Fixture *fixture, const gchar *body, gint depth, gboolean replies,
           const gchar *task_id)
{
	g_autoptr(ClawtMessage) message = clawt_message_new("beta", "alpha",
	                                                    body);
	g_autoptr(GError) error = NULL;

	clawt_message_set_depth(message, depth);
	clawt_message_set_invites_reply(message, replies);
	clawt_message_set_task_id(message, task_id);
	g_assert_cmpint(clawt_mailbox_router_send(fixture->router, message,
	                                          &error), ==, 1);
	g_assert_no_error(error);
}

/*
 * The whole path for one steered correction: the delegator's follow-up
 * goes out with a steer request and its mailbox item's id; libreclaw
 * answers delivered; the description folds into the running turn --
 * deeper depth, the reply flag -- and the next real turn in the room is
 * described by its own message, not by the steer.  And the turn records
 * join, so a restart has one turn to settle, not two.
 */
static void
test_router_steers_the_delegators_correction(void)
{
	Fixture fixture = { 0 };
	g_autofree gchar *room = NULL;
	g_autoptr(JsonObject) first = NULL;
	g_autoptr(JsonObject) steered = NULL;
	g_autoptr(JsonObject) answer = NULL;
	ClawtAgent *beta;
	ClawtMailbox *mailbox;
	const gchar *item_id;

	fixture_setup(&fixture, "agents:\n  - id: alpha\n  - id: beta\n");
	beta = clawt_agent_manager_get(fixture.agents, "beta");
	mailbox = clawt_agent_get_mailbox(beta);
	room = clawt_room_manager_direct_id("alpha", "beta");
	give_beta_a_link(&fixture, TRUE);

	/* alpha hands beta the work; beta's turn starts on it. */
	alpha_says(&fixture, "migrate the prod DB", 1, FALSE, NULL);
	first = read_one(&fixture);
	g_assert_false(json_object_has_member(first, "steer"));
	g_assert_nonnull(json_object_get_string_member(first, "message_id"));
	clawt_agent_note_typing(beta, room, TRUE);
	g_assert_cmpint(clawt_agent_get_hop_depth_in(beta, room), ==, 1);
	g_assert_false(clawt_agent_get_turn_replies_in(beta, room));
	g_assert_cmpuint(clawt_mailbox_turn_count_open(mailbox), ==, 1);

	/* The correction: deeper, and it wants an answer. */
	alpha_says(&fixture, "stop, use the staging DB", 3, TRUE, NULL);
	steered = read_one(&fixture);
	g_assert_true(json_object_get_boolean_member(steered, "steer"));
	item_id = json_object_get_string_member(steered, "message_id");
	g_assert_nonnull(item_id);
	g_assert_cmpuint(clawt_mailbox_turn_count_open(mailbox), ==, 2);

	/* Delivered: folded into the running turn, nothing left waiting. */
	g_assert_cmpint(clawt_agent_settle_steer(beta, room, item_id,
		"delivered", TRUE), ==, CLAWT_STEER_SETTLE_JOINED);
	g_assert_cmpint(clawt_agent_get_hop_depth_in(beta, room), ==, 3);
	g_assert_true(clawt_agent_get_turn_replies_in(beta, room));
	g_assert_cmpstr(clawt_agent_get_turn_origin_in(beta, room), ==, "alpha");

	/* Joined: one open record carrying both items. */
	g_assert_cmpuint(clawt_mailbox_turn_count_open(mailbox), ==, 1);
	{
		g_autoptr(GPtrArray) open = clawt_mailbox_turn_list_open(mailbox);
		ClawtTurnRecord *record = g_ptr_array_index(open, 0);

		g_assert_cmpuint(g_strv_length(record->items), ==, 2);
		g_assert_cmpstr(record->items[1], ==, item_id);
	}

	/* The turn ends: both close. */
	clawt_agent_note_typing(beta, room, FALSE);
	g_assert_cmpuint(clawt_mailbox_turn_count_open(mailbox), ==, 0);

	/*
	 * The next real turn is its own message's: an answer that closes
	 * the exchange, at depth 1.  Left queued, the steer's description
	 * would have been popped here, with its depth 3 and its open reply.
	 */
	alpha_says(&fixture, "thanks, done", 1, FALSE, NULL);
	answer = read_one(&fixture);
	g_assert_false(json_object_has_member(answer, "steer"));
	clawt_agent_note_typing(beta, room, TRUE);
	g_assert_cmpint(clawt_agent_get_hop_depth_in(beta, room), ==, 1);
	g_assert_false(clawt_agent_get_turn_replies_in(beta, room));

	fixture_teardown(&fixture);
}

/*
 * The first-reply rescue ("Try <model> instead") hands the running turn
 * back and interrupts.  With a delivery parked while libreclaw decides
 * whether to steer it, the room has two open records, and the parked
 * one is newer: the rescue must hand back the running turn's own record
 * (clawt_agent_get_turn_record_in()), not "the newest in the room".
 * And when the steer then answers delivered, the turn it joined is
 * gone: its record is closed rather than moved onto a deleted one, so
 * nothing reruns it and nothing is orphaned.
 */
static void
test_rescue_and_a_parked_steer(void)
{
	Fixture fixture = { 0 };
	g_autofree gchar *room = NULL;
	g_autoptr(JsonObject) first = NULL;
	g_autoptr(JsonObject) steered = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *first_id = NULL;
	g_autofree gchar *steer_id = NULL;
	ClawtAgent *beta;
	ClawtMailbox *mailbox;

	fixture_setup(&fixture, "agents:\n  - id: alpha\n  - id: beta\n");
	beta = clawt_agent_manager_get(fixture.agents, "beta");
	mailbox = clawt_agent_get_mailbox(beta);
	room = clawt_room_manager_direct_id("alpha", "beta");
	give_beta_a_link(&fixture, TRUE);

	alpha_says(&fixture, "do the thing", 1, FALSE, NULL);
	first = read_one(&fixture);
	first_id = g_strdup(json_object_get_string_member(first, "message_id"));
	clawt_agent_note_typing(beta, room, TRUE);

	alpha_says(&fixture, "use the staging DB", 1, TRUE, NULL);
	steered = read_one(&fixture);
	steer_id = g_strdup(json_object_get_string_member(steered, "message_id"));
	g_assert_true(json_object_get_boolean_member(steered, "steer"));
	g_assert_cmpuint(clawt_mailbox_turn_count_open(mailbox), ==, 2);

	/* The rescue hands back the running turn's record, by id. */
	g_assert_cmpint(clawt_mailbox_turn_hand_back_full(mailbox, room,
		clawt_agent_get_turn_record_in(beta, room), "haiku", &error), ==, 1);
	g_assert_no_error(error);
	{
		g_autoptr(ClawtMailboxItem) back = clawt_mailbox_get(mailbox, first_id);
		g_autoptr(ClawtMailboxItem) parked = clawt_mailbox_get(mailbox,
			steer_id);

		g_assert_cmpint(clawt_mailbox_item_get_state(back), ==,
			CLAWT_MAILBOX_PENDING);
		g_assert_cmpstr(clawt_mailbox_item_get_model(back), ==, "haiku");
		g_assert_cmpint(clawt_mailbox_item_get_state(parked), ==,
			CLAWT_MAILBOX_ACKED);
	}
	g_assert_cmpuint(clawt_mailbox_turn_count_open(mailbox), ==, 1);

	/* The steer lands after: its record closes, nothing is left over. */
	g_assert_cmpint(clawt_agent_settle_steer(beta, room, steer_id,
		"delivered", TRUE), ==, CLAWT_STEER_SETTLE_JOINED);
	g_assert_cmpuint(clawt_mailbox_turn_count_open(mailbox), ==, 0);

	fixture_teardown(&fixture);
}

/*
 * A refusal: the description waits for the message's own turn, and no
 * other turn takes it while the answer is pending.  Before asking it
 * keeps its place; after the engine refused, it goes to the back,
 * because that is where libreclaw queued the message.
 */
static void
test_refused_steer_waits_for_its_own_turn(void)
{
	Fixture fixture = { 0 };
	g_autofree gchar *room = NULL;
	g_autoptr(JsonObject) first = NULL;
	g_autoptr(JsonObject) steered = NULL;
	g_autoptr(JsonObject) behind = NULL;
	g_autofree gchar *item_id = NULL;
	ClawtAgent *beta;

	fixture_setup(&fixture, "agents:\n  - id: alpha\n  - id: beta\n");
	beta = clawt_agent_manager_get(fixture.agents, "beta");
	room = clawt_room_manager_direct_id("alpha", "beta");
	give_beta_a_link(&fixture, TRUE);

	alpha_says(&fixture, "do the thing", 1, FALSE, NULL);
	first = read_one(&fixture);
	clawt_agent_note_typing(beta, room, TRUE);

	/* A steer at depth 4 that wants an answer... */
	alpha_says(&fixture, "and this", 4, TRUE, NULL);
	steered = read_one(&fixture);
	item_id = g_strdup(json_object_get_string_member(steered, "message_id"));

	/*
	 * ...and an ordinary result behind it, which is not steered (it is
	 * an answer the router holds while the room is busy, so it is not
	 * even delivered yet).  Instead the turn ends and a new one starts
	 * before the answer arrives: it must not take the waiting steer.
	 */
	clawt_agent_note_typing(beta, room, FALSE);
	alpha_says(&fixture, "a plain one", 2, FALSE, NULL);
	behind = read_one(&fixture);
	g_assert_false(json_object_has_member(behind, "steer"));
	clawt_agent_note_typing(beta, room, TRUE);
	g_assert_cmpint(clawt_agent_get_hop_depth_in(beta, room), ==, 2);

	/* The engine refused: the steer goes to the back and waits. */
	g_assert_cmpint(clawt_agent_settle_steer(beta, room, item_id, "refused",
		TRUE), ==, CLAWT_STEER_SETTLE_QUEUED);
	clawt_agent_note_typing(beta, room, FALSE);
	clawt_agent_note_typing(beta, room, TRUE);
	g_assert_cmpint(clawt_agent_get_hop_depth_in(beta, room), ==, 4);
	g_assert_true(clawt_agent_get_turn_replies_in(beta, room));

	/* An answer for something no longer waiting changes nothing. */
	g_assert_cmpint(clawt_agent_settle_steer(beta, room, item_id,
		"delivered", TRUE), ==, CLAWT_STEER_SETTLE_UNKNOWN);

	fixture_teardown(&fixture);
}

/* Before asking, a refused steer keeps its place in the queue. */
static void
test_refused_before_asking_keeps_its_place(void)
{
	Fixture fixture = { 0 };
	ClawtAgent *agent = NULL;
	g_autoptr(ClawtMailboxItem) steer = NULL;
	g_autoptr(ClawtMailboxItem) after = NULL;

	fixture_setup(&fixture, "agents:\n  - id: alpha\n  - id: beta\n");
	agent = clawt_agent_manager_get(fixture.agents, "beta");

	steer = clawt_mailbox_item_new("alpha", "beta", "steer me");
	clawt_mailbox_item_set_id(steer, "item-s");
	clawt_mailbox_item_set_room(steer, "dm:alpha:beta");
	clawt_mailbox_item_set_depth(steer, 5);
	after = clawt_mailbox_item_new("alpha", "beta", "behind it");
	clawt_mailbox_item_set_id(after, "item-b");
	clawt_mailbox_item_set_room(after, "dm:alpha:beta");
	clawt_mailbox_item_set_depth(after, 2);

	clawt_agent_deliver_item(agent, steer, TRUE);
	g_assert_true(clawt_agent_await_steer(agent, "item-s"));
	clawt_agent_deliver_item(agent, after, TRUE);
	g_assert_false(clawt_agent_await_steer(agent, "item-none"));

	/* libreclaw queued it on arrival, ahead of the one behind it. */
	g_assert_cmpint(clawt_agent_settle_steer(agent, "dm:alpha:beta",
		"item-s", "refused", FALSE), ==, CLAWT_STEER_SETTLE_QUEUED);
	clawt_agent_note_typing(agent, "dm:alpha:beta", TRUE);
	g_assert_cmpint(clawt_agent_get_hop_depth_in(agent, "dm:alpha:beta"),
		==, 5);

	fixture_teardown(&fixture);
}

/*
 * An agent that ran the steered message as a turn without answering
 * (it went away mid-answer): with nothing else waiting for the room,
 * the turn takes the steer's description, since it is that message's.
 */
static void
test_unanswered_steer_is_taken_as_a_last_resort(void)
{
	Fixture fixture = { 0 };
	ClawtAgent *agent = NULL;
	g_autoptr(ClawtMailboxItem) steer = NULL;

	fixture_setup(&fixture, "agents:\n  - id: alpha\n  - id: beta\n");
	agent = clawt_agent_manager_get(fixture.agents, "beta");

	steer = clawt_mailbox_item_new("alpha", "beta", "steer me");
	clawt_mailbox_item_set_id(steer, "item-s");
	clawt_mailbox_item_set_room(steer, "dm:alpha:beta");
	clawt_mailbox_item_set_depth(steer, 3);
	clawt_agent_deliver_item(agent, steer, TRUE);
	g_assert_true(clawt_agent_await_steer(agent, "item-s"));

	clawt_agent_note_typing(agent, "dm:alpha:beta", TRUE);
	g_assert_cmpint(clawt_agent_get_hop_depth_in(agent, "dm:alpha:beta"),
		==, 3);

	fixture_teardown(&fixture);
}

/*
 * The fold, field by field, in the safe direction: a peer's origin
 * over the operator's, the deeper depth, replies if either, and the
 * task dropped when they disagree.  And an answer arriving after the
 * turn ended closes the steered part's record itself.
 */
static void
test_fold_is_in_the_safe_direction(void)
{
	Fixture fixture = { 0 };
	ClawtAgent *agent = NULL;
	g_autoptr(ClawtMailboxItem) start = NULL;
	g_autoptr(ClawtMailboxItem) steer = NULL;

	fixture_setup(&fixture, "agents:\n  - id: alpha\n  - id: beta\n");
	agent = clawt_agent_manager_get(fixture.agents, "beta");

	start = clawt_mailbox_item_new("user", "beta", "do it");
	clawt_mailbox_item_set_id(start, "item-1");
	clawt_mailbox_item_set_room(start, "dm:beta:user");
	clawt_mailbox_item_set_task_id(start, "task-1");
	clawt_agent_deliver_item(agent, start, FALSE);
	clawt_agent_note_typing(agent, "dm:beta:user", TRUE);

	steer = clawt_mailbox_item_new("alpha", "beta", "also");
	clawt_mailbox_item_set_id(steer, "item-2");
	clawt_mailbox_item_set_room(steer, "dm:beta:user");
	clawt_mailbox_item_set_depth(steer, 2);
	clawt_mailbox_item_set_task_id(steer, "task-2");
	clawt_agent_deliver_item(agent, steer, TRUE);
	g_assert_true(clawt_agent_await_steer(agent, "item-2"));

	g_assert_cmpint(clawt_agent_settle_steer(agent, "dm:beta:user",
		"item-2", "indeterminate", TRUE), ==, CLAWT_STEER_SETTLE_JOINED);
	g_assert_cmpint(clawt_agent_get_hop_depth_in(agent, "dm:beta:user"),
		==, 2);
	g_assert_true(clawt_agent_get_turn_replies_in(agent, "dm:beta:user"));
	g_assert_cmpstr(clawt_agent_get_turn_origin_in(agent, "dm:beta:user"),
		==, "alpha");
	{
		const gchar *task = "unset";

		g_assert_true(clawt_agent_describe_turn_in(agent, "dm:beta:user",
			NULL, &task, NULL));
		g_assert_null(task);
	}

	fixture_teardown(&fixture);
}

/* never, and an agent that does not steer: nothing asks. */
static void
test_router_asks_only_when_it_may(void)
{
	Fixture fixture = { 0 };
	g_autofree gchar *room = NULL;
	g_autoptr(JsonObject) first = NULL;
	g_autoptr(JsonObject) second = NULL;
	ClawtAgent *beta;

	/* An agent at never. */
	fixture_setup(&fixture,
		"agents:\n  - id: alpha\n  - id: beta\n    steer: never\n");
	beta = clawt_agent_manager_get(fixture.agents, "beta");
	room = clawt_room_manager_direct_id("alpha", "beta");
	give_beta_a_link(&fixture, TRUE);

	alpha_says(&fixture, "go", 1, FALSE, NULL);
	first = read_one(&fixture);
	clawt_agent_note_typing(beta, room, TRUE);
	alpha_says(&fixture, "and this", 1, TRUE, NULL);
	second = read_one(&fixture);
	g_assert_false(json_object_has_member(second, "steer"));
	fixture_teardown(&fixture);
	g_clear_pointer(&first, json_object_unref);
	g_clear_pointer(&second, json_object_unref);

	/* An agent whose link never said it steers. */
	fixture_setup(&fixture, "agents:\n  - id: alpha\n  - id: beta\n");
	beta = clawt_agent_manager_get(fixture.agents, "beta");
	give_beta_a_link(&fixture, FALSE);

	alpha_says(&fixture, "go", 1, FALSE, NULL);
	first = read_one(&fixture);
	clawt_agent_note_typing(beta, room, TRUE);
	alpha_says(&fixture, "and this", 1, TRUE, NULL);
	second = read_one(&fixture);
	g_assert_false(json_object_has_member(second, "steer"));
	/* The id still rides, for anything that wants to name it. */
	g_assert_nonnull(json_object_get_string_member(second, "message_id"));

	/* Another task is a turn of its own, even from the delegator. */
	clawt_link_set_can_steer(clawt_agent_get_link(beta), TRUE);
	g_clear_pointer(&second, json_object_unref);
	alpha_says(&fixture, "a new job", 1, TRUE, "task-other");
	second = read_one(&fixture);
	g_assert_false(json_object_has_member(second, "steer"));

	fixture_teardown(&fixture);
}

/* ── The room mark ───────────────────────────────────────────────── */

/*
 * Drawn as steered: the mark lands on the message, in the history a
 * client reads, and survives a restart as a line of its own -- the
 * transcript is append-only -- without becoming a message.
 */
static void
test_room_mark_survives_a_restart(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("clawt-steer-room-XXXXXX", NULL);
	g_autofree gchar *id = NULL;

	{
		g_autoptr(ClawtRoomManager) rooms = clawt_room_manager_new(dir);
		g_autoptr(ClawtMessage) message = NULL;
		g_autoptr(GError) error = NULL;
		ClawtRoom *room = clawt_room_manager_get_direct(rooms, "alpha",
			"beta");

		message = clawt_message_new(clawt_room_get_id(room), "alpha",
			"stop, use the staging DB");
		g_assert_true(clawt_room_append(room, message, &error));
		g_assert_no_error(error);
		id = g_strdup(clawt_message_get_id(message));
		g_assert_nonnull(id);

		g_assert_true(clawt_room_mark_steered(room, id, "delivered", &error));
		g_assert_no_error(error);
		/* Once: a second answer is not a second line. */
		g_assert_true(clawt_room_mark_steered(room, id, "indeterminate",
			&error));
		g_assert_false(clawt_room_mark_steered(room, "no-such", "delivered",
			&error));
	}

	{
		g_autoptr(ClawtRoomManager) rooms = clawt_room_manager_new(dir);
		g_autoptr(GPtrArray) history = NULL;
		g_autoptr(GError) error = NULL;
		ClawtRoom *room;
		ClawtMessage *back;

		g_assert_cmpuint(clawt_room_manager_load_direct(rooms), >=, 1);
		room = clawt_room_manager_get_direct(rooms, "alpha", "beta");
		history = clawt_room_get_history(room, 0);

		/* One message, not a message and an annotation. */
		g_assert_cmpuint(history->len, ==, 1);
		back = g_ptr_array_index(history, 0);
		g_assert_cmpstr(clawt_message_get_id(back), ==, id);
		g_assert_cmpstr(clawt_message_get_steered(back), ==, "delivered");
	}

	clawt_test_remove_tree(dir);
}

/* ── The link ────────────────────────────────────────────────────── */

typedef struct {
	gchar    *room;
	gchar    *item;
	gchar    *outcome;
	gchar    *reason;
	gboolean  asked;
	gboolean  seen;
	gboolean  added;
} SteeredSeen;

static void
on_server_steered(ClawtLinkServer *server, const gchar *agent_id,
                  const gchar *room, const gchar *item, const gchar *outcome,
                  const gchar *reason, gboolean asked, gpointer user_data)
{
	SteeredSeen *seen = user_data;

	(void)server;
	g_assert_cmpstr(agent_id, ==, "beta");
	seen->room = g_strdup(room);
	seen->item = g_strdup(item);
	seen->outcome = g_strdup(outcome);
	seen->reason = g_strdup(reason);
	seen->asked = asked;
	seen->seen = TRUE;
}

static void
on_link_added(ClawtLinkServer *server, const gchar *agent_id,
              gpointer user_data)
{
	(void)server;
	(void)agent_id;
	((SteeredSeen *)user_data)->added = TRUE;
}

static void
pump_until_flag(gboolean *flag)
{
	guint waited = 0;

	while (!*flag && waited < 3000) {
		g_main_context_iteration(NULL, FALSE);
		g_usleep(1000);
		waited++;
	}
	g_assert_true(*flag);
}

static void
client_line(GOutputStream *out, const gchar *line)
{
	g_autoptr(GError) error = NULL;

	g_output_stream_write_all(out, line, strlen(line), NULL, NULL, &error);
	g_assert_no_error(error);
	g_output_stream_write_all(out, "\n", 1, NULL, NULL, &error);
	g_assert_no_error(error);
}

/*
 * The hello's features decide whether the link steers, and chat.steered
 * reaches the server's subscribers with the agent it came from.
 */
static void
test_link_hello_and_answer(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("clawt-steer-link-XXXXXX", NULL);
	g_autofree gchar *path = g_build_filename(dir, "agents.sock", NULL);
	g_autoptr(ClawtLinkServer) server = clawt_link_server_new(path);
	g_autoptr(GSocketClient) socket_client = g_socket_client_new();
	g_autoptr(GSocketAddress) address = NULL;
	g_autoptr(GSocketConnection) connection = NULL;
	g_autoptr(GError) error = NULL;
	SteeredSeen seen;
	GOutputStream *out;

	memset(&seen, 0, sizeof(seen));
	g_signal_connect(server, "link-added", G_CALLBACK(on_link_added), &seen);
	g_signal_connect(server, "steered", G_CALLBACK(on_server_steered),
		&seen);
	g_assert_true(clawt_link_server_start(server, &error));
	g_assert_no_error(error);

	address = g_unix_socket_address_new(path);
	connection = g_socket_client_connect(socket_client,
		G_SOCKET_CONNECTABLE(address), NULL, &error);
	g_assert_no_error(error);
	out = g_io_stream_get_output_stream(G_IO_STREAM(connection));

	client_line(out, "{\"v\":1,\"kind\":\"control.hello\",\"payload\":"
		"{\"agent_id\":\"beta\",\"features\":[\"voice\",\"steer\"]}}");
	pump_until_flag(&seen.added);
	g_assert_true(clawt_link_get_can_steer(
		clawt_link_server_get_link(server, "beta")));

	client_line(out, "{\"v\":1,\"kind\":\"chat.steered\",\"payload\":{"
		"\"room_id\":\"dm:alpha:beta\",\"message_id\":\"item-7\","
		"\"outcome\":\"refused\",\"reason\":\"no turn is running\","
		"\"asked\":false}}");
	pump_until_flag(&seen.seen);
	g_assert_cmpstr(seen.room, ==, "dm:alpha:beta");
	g_assert_cmpstr(seen.item, ==, "item-7");
	g_assert_cmpstr(seen.outcome, ==, "refused");
	g_assert_cmpstr(seen.reason, ==, "no turn is running");
	g_assert_false(seen.asked);

	g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
	clawt_link_server_stop(server);
	g_unlink(path);
	clawt_test_remove_tree(dir);

	g_free(seen.room);
	g_free(seen.item);
	g_free(seen.outcome);
	g_free(seen.reason);
}

/* A hello without the feature: never asked, and its answers ignored. */
static void
test_link_without_the_feature(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("clawt-steer-link-XXXXXX", NULL);
	g_autofree gchar *path = g_build_filename(dir, "agents.sock", NULL);
	g_autoptr(ClawtLinkServer) server = clawt_link_server_new(path);
	g_autoptr(GSocketClient) socket_client = g_socket_client_new();
	g_autoptr(GSocketAddress) address = NULL;
	g_autoptr(GSocketConnection) connection = NULL;
	g_autoptr(GError) error = NULL;
	SteeredSeen seen;
	GOutputStream *out;
	guint i;

	memset(&seen, 0, sizeof(seen));
	g_signal_connect(server, "link-added", G_CALLBACK(on_link_added), &seen);
	g_signal_connect(server, "steered", G_CALLBACK(on_server_steered),
		&seen);
	g_assert_true(clawt_link_server_start(server, &error));

	address = g_unix_socket_address_new(path);
	connection = g_socket_client_connect(socket_client,
		G_SOCKET_CONNECTABLE(address), NULL, &error);
	g_assert_no_error(error);
	out = g_io_stream_get_output_stream(G_IO_STREAM(connection));

	client_line(out, "{\"v\":1,\"kind\":\"control.hello\",\"payload\":"
		"{\"agent_id\":\"beta\"}}");
	pump_until_flag(&seen.added);
	g_assert_false(clawt_link_get_can_steer(
		clawt_link_server_get_link(server, "beta")));

	client_line(out, "{\"v\":1,\"kind\":\"chat.steered\",\"payload\":{"
		"\"message_id\":\"item-7\",\"outcome\":\"delivered\"}}");
	for (i = 0; i < 200; i++) {
		g_main_context_iteration(NULL, FALSE);
		g_usleep(1000);
	}
	g_assert_false(seen.seen);

	g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
	clawt_link_server_stop(server);
	g_unlink(path);
	clawt_test_remove_tree(dir);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);

	g_test_add_func("/steering/policy/levels", test_policy_levels);
	g_test_add_func("/steering/policy/hard-rules", test_policy_hard_rules);
	g_test_add_func("/steering/policy/vocabulary", test_policy_vocabulary);
	g_test_add_func("/steering/router/delegators-correction",
		test_router_steers_the_delegators_correction);
	g_test_add_func("/steering/router/asks-only-when-it-may",
		test_router_asks_only_when_it_may);
	g_test_add_func("/steering/turn/rescue-and-a-parked-steer",
		test_rescue_and_a_parked_steer);
	g_test_add_func("/steering/turn/refused-waits",
		test_refused_steer_waits_for_its_own_turn);
	g_test_add_func("/steering/turn/refused-before-asking",
		test_refused_before_asking_keeps_its_place);
	g_test_add_func("/steering/turn/last-resort",
		test_unanswered_steer_is_taken_as_a_last_resort);
	g_test_add_func("/steering/turn/fold", test_fold_is_in_the_safe_direction);
	g_test_add_func("/steering/room/mark", test_room_mark_survives_a_restart);
	g_test_add_func("/steering/link/hello-and-answer",
		test_link_hello_and_answer);
	g_test_add_func("/steering/link/without-the-feature",
		test_link_without_the_feature);

	return g_test_run();
}
