/*
 * test-room-rounds.c - Rounds in a group room, and a budget per task tree
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Two limits on how much agents talk that max_hops alone did not give:
 *
 *  - a group room counts in *rounds*.  A round starts at a message from
 *    a person, a routine or a trigger; within one thread of replies a
 *    member answers once, and a thread stops at rooms.max_chain.  The
 *    hop depth starts again at each round, so an ordinary standup no
 *    longer reaches max_hops by itself -- 111 refusals in one session,
 *    every one of them a room.
 *  - a task tree has orchestration.tree_message_budget requests between
 *    settlements, counted once per call however many it reaches.
 *
 * Every test that touches keyed state uses two keys -- two rooms, two
 * trees -- because a test with one key cannot see a shared-scalar bug.
 *
 * The daemon half includes core/clawt-daemon-private.h for the reason
 * test-group-room.c does: the link server's signals and the daemon's
 * own guard are the interface being tested.
 */

#include <clawtilla.h>

#include <gio/gio.h>
#include <glib/gstdio.h>

#include <sqlite3.h>
#include <string.h>
#include <sys/socket.h>

#include "clawt-test-util.h"

#include "core/clawt-daemon-private.h"

/* ── The chain arithmetic ────────────────────────────────────────── */

static void
test_chain_helpers(void)
{
	g_autofree gchar *one = clawt_round_chain_extend(NULL, "alpha");
	g_autofree gchar *two = clawt_round_chain_extend(one, "beta");
	g_autofree gchar *same = clawt_round_chain_extend(two, "alpha");
	g_autofree gchar *both = clawt_round_chain_union("alpha,beta",
	                                                 "gamma,beta");
	g_autofree gchar *none = clawt_round_chain_union(NULL, "");
	g_autofree gchar *label = clawt_round_label(4);
	g_autofree gchar *note = NULL;

	g_assert_cmpstr(one, ==, "alpha");
	g_assert_cmpstr(two, ==, "alpha,beta");
	g_assert_cmpstr(same, ==, "alpha,beta");
	g_assert_cmpstr(both, ==, "alpha,beta,gamma");
	g_assert_null(none);

	g_assert_cmpuint(clawt_round_chain_length(NULL), ==, 0);
	g_assert_cmpuint(clawt_round_chain_length(""), ==, 0);
	g_assert_cmpuint(clawt_round_chain_length(two), ==, 2);

	/* A whole entry, never a prefix: "bob" is not in "bobby". */
	g_assert_true(clawt_round_chain_has("bobby,bob", "bob"));
	g_assert_false(clawt_round_chain_has("bobby,carol", "bob"));
	g_assert_false(clawt_round_chain_has(NULL, "bob"));

	g_assert_cmpstr(label, ==, "Round 4");

	g_assert_null(clawt_round_skip_note(NULL, "why"));
	note = clawt_round_skip_note("alpha,beta,gamma", "they answered");
	g_assert_cmpstr(note, ==,
	                "Not delivered to @alpha, @beta and @gamma \xe2\x80\x94 "
	                "they answered");
	g_assert_nonnull(strstr(clawt_round_skip_tooltip(), "rooms.max_chain"));
}

/* ── The router: a fixture with links and a guard ────────────────── */

typedef struct {
	gchar              *dir;
	ClawtConfig        *config;
	ClawtAgentManager  *agents;
	ClawtRoomManager   *rooms;
	ClawtLoopGuard     *guard;
	ClawtMailboxRouter *router;
	GHashTable         *far_ends;   /* agent id -> GSocket */
} Fixture;

static gboolean
is_agent(const gchar *sender_id, gpointer user_data)
{
	Fixture *fixture = user_data;

	return sender_id != NULL &&
	       clawt_agent_manager_get(fixture->agents, sender_id) != NULL;
}

static void
fixture_setup(Fixture *fixture, const gchar *agents_yaml, guint max_hops)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *yaml = NULL;
	g_autofree gchar *transcripts = NULL;

	fixture->dir = g_dir_make_tmp("clawt-rounds-XXXXXX", NULL);
	transcripts = g_build_filename(fixture->dir, "rooms", NULL);

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

	fixture->rooms = clawt_room_manager_new(transcripts);

	/* Hops only: the rate and cycle checks are other tests' business. */
	fixture->guard = clawt_loop_guard_new();
	clawt_loop_guard_set_limits(fixture->guard, max_hops, 0, 0);
	clawt_loop_guard_set_task_budget(fixture->guard, 0.0);
	clawt_loop_guard_set_peer_func(fixture->guard, is_agent, fixture, NULL);

	fixture->router = clawt_mailbox_router_new(fixture->agents,
	                                           fixture->rooms,
	                                           fixture->guard);
	fixture->far_ends = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                          g_free, g_object_unref);
}

static void
fixture_teardown(Fixture *fixture)
{
	g_clear_pointer(&fixture->far_ends, g_hash_table_unref);
	g_clear_object(&fixture->router);
	if (fixture->guard != NULL)
		clawt_loop_guard_set_peer_func(fixture->guard, NULL, NULL, NULL);
	g_clear_object(&fixture->guard);
	g_clear_object(&fixture->rooms);
	g_clear_object(&fixture->agents);
	g_clear_object(&fixture->config);

	if (fixture->dir != NULL)
		clawt_test_remove_tree(fixture->dir);

	g_clear_pointer(&fixture->dir, g_free);
}

/* A socketpair the drain really writes into, read back per agent. */
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

	g_assert_nonnull(agent);
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

	g_assert_nonnull(far_end);
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

static ClawtRoom *
make_group(Fixture *fixture, const gchar *room_id, guint max_chain)
{
	g_autoptr(GError) error = NULL;
	ClawtRoom *room;

	room = clawt_room_manager_create(fixture->rooms, room_id, NULL, &error);
	g_assert_no_error(error);
	clawt_room_add_member(room, "alpha");
	clawt_room_add_member(room, "beta");
	clawt_room_add_member(room, "gamma");
	clawt_room_set_require_mention(room, TRUE);
	clawt_room_set_max_chain(room, max_chain);

	return room;
}

/* A person speaking in @room: what msg.send does. */
static void
person_says(Fixture *fixture, const gchar *room_id, const gchar *body)
{
	g_autoptr(GError) error = NULL;

	g_assert_cmpint(clawt_mailbox_router_send_to(fixture->router, "user",
		room_id, body, NULL, 0, &error), >=, 0);
	g_assert_no_error(error);
}

/*
 * @agent_id takes the turn waiting for it in @room and ends it with
 * @body: what the daemon's link handler routes -- one hop past the turn,
 * inviting a reply when it names a member of a mention room.
 *
 * Returns: what the router answered, -1 for a refusal
 */
static gint
agent_replies(Fixture *fixture, const gchar *agent_id, const gchar *room_id,
              const gchar *body, GError **error)
{
	ClawtAgent *agent = clawt_agent_manager_get(fixture->agents, agent_id);
	ClawtRoom *room = clawt_room_manager_get(fixture->rooms, room_id);
	g_autoptr(ClawtMessage) message = clawt_message_new(room_id, agent_id,
	                                                    body);

	clawt_agent_begin_turn(agent, room_id);
	clawt_message_set_depth(message,
		clawt_agent_get_hop_depth_in(agent, room_id) + 1);
	clawt_message_set_invites_reply(message,
		clawt_room_names_any_member(room, body, fixture->agents));

	return clawt_mailbox_router_send(fixture->router, message, error);
}

/* How many deliveries @agent_id was handed in @room since last asked. */
static guint
deliveries(Fixture *fixture, const gchar *agent_id, const gchar *room_id)
{
	g_autofree gchar *text = read_all(fixture, agent_id);
	g_autofree gchar *marker = g_strdup_printf(
		"The following was posted in room '%s'", room_id);

	return clawt_test_count_substrings(text, marker);
}

static const gchar *TEAM[] = { "alpha", "beta", "gamma" };

/*
 * Runs one round of @room to the end: a person opens it with @all, and
 * every member that is reached answers by naming both of the others --
 * the worst a standup can do.  Returns how many replies it took, and
 * fails on any refusal.
 */
static guint
run_a_round(Fixture *fixture, const gchar *room_id, guint round_no)
{
	GQueue turns = G_QUEUE_INIT;
	guint replies = 0;
	guint i;

	{
		g_autofree gchar *open = g_strdup_printf(
			"@all standup %u: what are you on?", round_no);

		person_says(fixture, room_id, open);
	}

	for (i = 0; i < G_N_ELEMENTS(TEAM); i++) {
		guint n = deliveries(fixture, TEAM[i], room_id);

		while (n-- > 0)
			g_queue_push_tail(&turns, (gpointer)TEAM[i]);
	}

	while (!g_queue_is_empty(&turns)) {
		const gchar *who = g_queue_pop_head(&turns);
		g_autoptr(GString) body = g_string_new(NULL);
		g_autoptr(GError) error = NULL;

		/* A runaway would not end; a test that hangs is worse. */
		g_assert_cmpuint(replies, <, 64);

		g_string_append_printf(body, "%s here, round %u.", who, round_no);
		for (i = 0; i < G_N_ELEMENTS(TEAM); i++) {
			if (g_strcmp0(TEAM[i], who) != 0)
				g_string_append_printf(body, " @%s what about you?",
				                       TEAM[i]);
		}

		g_assert_cmpint(agent_replies(fixture, who, room_id, body->str,
		                              &error), >=, 0);
		g_assert_no_error(error);
		replies++;

		for (i = 0; i < G_N_ELEMENTS(TEAM); i++) {
			guint n = deliveries(fixture, TEAM[i], room_id);

			while (n-- > 0)
				g_queue_push_tail(&turns, (gpointer)TEAM[i]);
		}
	}

	return replies;
}

/*
 * The standup that used to reach the ceiling by itself.
 *
 * Three members who each name both of the others on every reply, in two
 * rooms at once, for twenty rounds, with max_hops at 4.  Before rounds
 * the depth climbed with every reply and the room was refused within
 * the first round; now each thread ends at rooms.max_chain, the depth
 * starts again at every round, and nothing is ever refused.  Both rooms
 * are numbered on their own, and every round is bounded.
 */
static void
test_many_rounds_never_reach_max_hops(void)
{
	Fixture fixture = { 0 };
	ClawtRoom *first;
	ClawtRoom *second;
	guint round_no;
	guint i;

	fixture_setup(&fixture,
		"agents:\n  - id: alpha\n  - id: beta\n  - id: gamma\n", 4);
	for (i = 0; i < G_N_ELEMENTS(TEAM); i++)
		give_agent_a_link(&fixture, TEAM[i]);

	first = make_group(&fixture, "standup-a", 3);
	second = make_group(&fixture, "standup-b", 3);

	for (round_no = 1; round_no <= 20; round_no++) {
		guint in_first = run_a_round(&fixture, "standup-a", round_no);
		guint in_second;

		/*
		 * Interleaved, and the second room started mid-way, so the
		 * two rooms' round numbers differ: a shared counter would
		 * number them the same.
		 */
		if (round_no > 5)
			in_second = run_a_round(&fixture, "standup-b", round_no);
		else
			in_second = 0;

		/*
		 * Three openers, each thread at most three replies long: a
		 * round is bounded whatever the members do.
		 */
		g_assert_cmpuint(in_first, <=, 3 * 3 * 3);
		g_assert_cmpuint(in_first, >=, 3);
		if (round_no > 5)
			g_assert_cmpuint(in_second, ==, in_first);
	}

	g_assert_cmpuint(clawt_room_get_round(first), ==, 20);
	g_assert_cmpuint(clawt_room_get_round(second), ==, 15);

	/* Never deeper than a thread, and some mentions were withheld. */
	{
		g_autoptr(GPtrArray) history = clawt_room_get_history(first, 0);
		guint withheld = 0;
		gint deepest = 0;

		for (i = 0; i < history->len; i++) {
			ClawtMessage *message = g_ptr_array_index(history, i);

			deepest = MAX(deepest, clawt_message_get_depth(message));
			if (clawt_message_get_skipped(message) != NULL)
				withheld++;
			g_assert_cmpuint(clawt_message_get_round(message), >, 0);
		}

		g_assert_cmpint(deepest, <=, 3);
		g_assert_cmpuint(withheld, >, 0);
	}

	fixture_teardown(&fixture);
}

/*
 * A mention of a member already in its own thread is recorded and not
 * delivered, and says why -- in the message, the transcript, and the
 * event a client draws it from.  A member that spoke in *another*
 * thread of the same round still answers.
 */
static void
test_a_mention_inside_its_own_thread_is_withheld(void)
{
	Fixture fixture = { 0 };
	g_autoptr(ClawtEventBus) bus = clawt_event_bus_new(64);
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) history = NULL;
	g_autoptr(GPtrArray) events = NULL;
	ClawtRoom *room;
	ClawtMessage *last;
	gboolean event_seen = FALSE;
	guint i;

	fixture_setup(&fixture,
		"agents:\n  - id: alpha\n  - id: beta\n  - id: gamma\n"
		"  - id: delta\n", 8);
	for (i = 0; i < G_N_ELEMENTS(TEAM); i++)
		give_agent_a_link(&fixture, TEAM[i]);
	give_agent_a_link(&fixture, "delta");
	clawt_mailbox_router_set_event_bus(fixture.router, bus);
	room = make_group(&fixture, "standup", 3);
	clawt_room_add_member(room, "delta");

	/* The person names alpha and gamma: two threads open. */
	person_says(&fixture, "standup", "@alpha @gamma status please");
	g_assert_cmpuint(deliveries(&fixture, "alpha", "standup"), ==, 1);
	g_assert_cmpuint(deliveries(&fixture, "gamma", "standup"), ==, 1);

	/* gamma answers its own thread first, naming nobody. */
	g_assert_cmpint(agent_replies(&fixture, "gamma", "standup",
		"all green here", &error), ==, 0);

	/* alpha's thread: alpha -> beta. */
	g_assert_cmpint(agent_replies(&fixture, "alpha", "standup",
		"@beta can you check the deploy?", &error), ==, 1);
	g_assert_cmpuint(deliveries(&fixture, "beta", "standup"), ==, 1);

	/*
	 * beta names alpha (already in this thread) and gamma (who spoke,
	 * but in the other thread): gamma is reached, alpha is not.
	 */
	g_assert_cmpint(agent_replies(&fixture, "beta", "standup",
		"@alpha done. @gamma FYI the deploy is out", &error), ==, 1);
	g_assert_no_error(error);
	g_assert_cmpuint(deliveries(&fixture, "alpha", "standup"), ==, 0);
	g_assert_cmpuint(deliveries(&fixture, "gamma", "standup"), ==, 1);

	history = clawt_room_get_history(room, 1);
	last = g_ptr_array_index(history, 0);
	g_assert_cmpstr(clawt_message_get_skipped(last), ==, "alpha");
	g_assert_nonnull(strstr(clawt_message_get_skip_reason(last),
	                        "already answered in this thread"));
	g_assert_cmpstr(clawt_message_get_chain(last), ==, "alpha,beta");
	g_assert_cmpuint(clawt_message_get_round(last), ==, 1);

	/*
	 * The thread's third reply reaches nobody: delta has not answered
	 * anywhere, but the thread is rooms.max_chain (3) replies long.
	 */
	g_assert_cmpint(agent_replies(&fixture, "gamma", "standup",
		"@delta can you double-check?", &error), ==, 0);
	g_assert_cmpuint(deliveries(&fixture, "delta", "standup"), ==, 0);
	g_clear_pointer(&history, g_ptr_array_unref);
	history = clawt_room_get_history(room, 1);
	last = g_ptr_array_index(history, 0);
	g_assert_cmpstr(clawt_message_get_chain(last), ==, "alpha,beta,gamma");
	g_assert_cmpstr(clawt_message_get_skipped(last), ==, "delta");
	g_assert_nonnull(strstr(clawt_message_get_skip_reason(last),
	                        "rooms.max_chain (3)"));

	/* The event carries it too, for a client drawing it live. */
	events = clawt_event_bus_replay(bus, 0, NULL);
	for (i = 0; i < events->len; i++) {
		ClawtEvent *event = g_ptr_array_index(events, i);

		if (g_strcmp0(clawt_event_get_kind(event), "message") == 0 &&
			g_strcmp0(clawt_event_get_detail(event, "skipped"),
			          "alpha") == 0) {
			g_assert_cmpint(clawt_event_get_detail_int(event, "round"),
			                ==, 1);
			g_assert_nonnull(clawt_event_get_detail(event, "skip_reason"));
			event_seen = TRUE;
		}
	}
	g_assert_true(event_seen);

	/* And the transcript keeps it across a restart. */
	{
		g_autofree gchar *transcripts = g_build_filename(fixture.dir,
		                                                 "rooms", NULL);
		g_autoptr(ClawtRoomManager) again = clawt_room_manager_new(
			transcripts);
		g_autoptr(GError) create_error = NULL;
		ClawtRoom *reloaded = clawt_room_manager_create(again, "standup",
		                                                NULL, &create_error);
		g_autoptr(GPtrArray) restored = NULL;
		gboolean found = FALSE;

		g_assert_no_error(create_error);
		g_assert_cmpuint(clawt_room_get_round(reloaded), ==, 1);
		restored = clawt_room_get_history(reloaded, 0);
		for (i = 0; i < restored->len; i++) {
			ClawtMessage *message = g_ptr_array_index(restored, i);

			if (g_strcmp0(clawt_message_get_skipped(message), "alpha") == 0) {
				g_assert_cmpstr(clawt_message_get_chain(message), ==,
				                "alpha,beta");
				found = TRUE;
			}
		}
		g_assert_true(found);
	}

	clawt_mailbox_router_set_event_bus(fixture.router, NULL);
	fixture_teardown(&fixture);
}

/*
 * Every message from a person, a routine or a trigger starts a round at
 * depth 0, whatever depth its sender stamped -- that is the reset.  The
 * daemon's own notice belongs to no round, and a pair is never numbered.
 */
static void
test_each_round_resets_the_depth(void)
{
	Fixture fixture = { 0 };
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) history = NULL;
	g_autoptr(ClawtMessage) routine = NULL;
	g_autoptr(ClawtMessage) notice = NULL;
	ClawtRoom *room;
	ClawtRoom *pair;
	ClawtMessage *last;
	guint i;

	fixture_setup(&fixture,
		"agents:\n  - id: alpha\n  - id: beta\n  - id: gamma\n", 8);
	for (i = 0; i < G_N_ELEMENTS(TEAM); i++)
		give_agent_a_link(&fixture, TEAM[i]);
	room = make_group(&fixture, "standup", 3);

	person_says(&fixture, "standup", "@alpha go");
	g_assert_cmpint(agent_replies(&fixture, "alpha", "standup",
		"@beta your turn", &error), ==, 1);
	g_assert_cmpint(agent_replies(&fixture, "beta", "standup",
		"@gamma yours", &error), ==, 1);
	g_assert_cmpuint(clawt_room_get_round(room), ==, 1);

	/* A routine speaking at depth 7 opens round 2 at depth 0. */
	routine = clawt_message_new("standup", "routine", "@alpha nightly check");
	clawt_message_set_depth(routine, 7);
	g_assert_cmpint(clawt_mailbox_router_send(fixture.router, routine,
	                                          &error), ==, 1);
	g_assert_cmpuint(clawt_room_get_round(room), ==, 2);

	history = clawt_room_get_history(room, 1);
	last = g_ptr_array_index(history, 0);
	g_assert_cmpint(clawt_message_get_depth(last), ==, 0);
	g_assert_cmpuint(clawt_message_get_round(last), ==, 2);
	g_assert_null(clawt_message_get_chain(last));
	g_clear_pointer(&history, g_ptr_array_unref);

	/* alpha's answer in round 2 is one hop deep again, not four. */
	(void)deliveries(&fixture, "alpha", "standup");
	g_assert_cmpint(agent_replies(&fixture, "alpha", "standup",
		"nightly is green", &error), ==, 0);
	history = clawt_room_get_history(room, 1);
	last = g_ptr_array_index(history, 0);
	g_assert_cmpint(clawt_message_get_depth(last), ==, 1);
	g_assert_cmpuint(clawt_message_get_round(last), ==, 2);
	g_assert_cmpstr(clawt_message_get_chain(last), ==, "alpha");
	g_clear_pointer(&history, g_ptr_array_unref);

	/* The daemon's notice opens nothing. */
	notice = clawt_message_new("standup", CLAWT_SYSTEM_SENDER,
	                           "[clawtilla] t-1 is finished.");
	clawt_message_set_only_for(notice, "alpha");
	clawt_message_set_invites_reply(notice, FALSE);
	g_assert_cmpint(clawt_mailbox_router_send(fixture.router, notice,
	                                          &error), ==, 1);
	g_assert_cmpuint(clawt_room_get_round(room), ==, 2);

	/* A pair has no rounds: the invite rule already ends its exchanges. */
	pair = clawt_room_manager_get_direct(fixture.rooms, "alpha", "user");
	person_says(&fixture, clawt_room_get_id(pair), "hello");
	g_assert_cmpuint(clawt_room_get_round(pair), ==, 0);

	fixture_teardown(&fixture);
}

/*
 * max_hops stays as the backstop.  With rooms.max_chain off, a thread
 * of five members runs until the once-per-thread rule ends it -- and
 * with max_hops at 3 the hop check refuses it first, as it always did.
 */
static void
test_max_hops_is_still_the_backstop(void)
{
	Fixture fixture = { 0 };
	g_autoptr(GError) error = NULL;
	ClawtRoom *room;
	static const gchar *five[] = { "a1", "a2", "a3", "a4", "a5" };
	guint i;

	fixture_setup(&fixture,
		"agents:\n  - id: a1\n  - id: a2\n  - id: a3\n  - id: a4\n"
		"  - id: a5\n", 3);
	for (i = 0; i < G_N_ELEMENTS(five); i++)
		give_agent_a_link(&fixture, five[i]);

	room = clawt_room_manager_create(fixture.rooms, "big", NULL, &error);
	g_assert_no_error(error);
	for (i = 0; i < G_N_ELEMENTS(five); i++)
		clawt_room_add_member(room, five[i]);
	clawt_room_set_require_mention(room, TRUE);
	clawt_room_set_max_chain(room, 0);

	person_says(&fixture, "big", "@a1 start");
	g_assert_cmpint(agent_replies(&fixture, "a1", "big", "@a2 next",
	                              &error), ==, 1);
	g_assert_cmpint(agent_replies(&fixture, "a2", "big", "@a3 next",
	                              &error), ==, 1);

	/* a3 answers at depth 3, which max_hops 3 refuses. */
	g_assert_cmpint(agent_replies(&fixture, "a3", "big", "@a4 next",
	                              &error), ==, -1);
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT);

	fixture_teardown(&fixture);
}

/* The round and the thread survive in a mailbox, and an old one migrates. */
static void
test_round_survives_in_the_mailbox(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("clawt-rounds-XXXXXX", NULL);
	g_autofree gchar *path = g_build_filename(dir, "mailbox.db", NULL);
	g_autofree gchar *old_path = g_build_filename(dir, "old.db", NULL);
	g_autofree gchar *id = NULL;

	{
		g_autoptr(ClawtMailbox) mailbox = NULL;
		g_autoptr(ClawtMailboxItem) item = NULL;
		g_autoptr(GError) error = NULL;

		mailbox = clawt_mailbox_new("beta", path, &error);
		g_assert_no_error(error);
		item = clawt_mailbox_item_new("alpha", "beta", "@beta your turn");
		clawt_mailbox_item_set_round_chain(item, 7, "alpha");
		id = clawt_mailbox_post(mailbox, item, &error);
		g_assert_no_error(error);
	}

	{
		g_autoptr(ClawtMailbox) mailbox = NULL;
		g_autoptr(ClawtMailboxItem) item = NULL;
		g_autoptr(ClawtMailboxItem) copy = NULL;
		g_autoptr(GError) error = NULL;

		mailbox = clawt_mailbox_new("beta", path, &error);
		g_assert_no_error(error);
		item = clawt_mailbox_get(mailbox, id);
		g_assert_nonnull(item);
		g_assert_cmpuint(clawt_mailbox_item_get_round(item), ==, 7);
		g_assert_cmpstr(clawt_mailbox_item_get_chain(item), ==, "alpha");

		copy = clawt_mailbox_item_copy(item);
		g_assert_cmpstr(clawt_mailbox_item_get_chain(copy), ==, "alpha");
	}

	{
		g_autoptr(ClawtMailbox) mailbox = NULL;
		g_autoptr(ClawtMailboxItem) item = NULL;
		g_autoptr(GError) error = NULL;
		sqlite3 *db = NULL;

		/* The schema as of 0.17.0: everything but the round. */
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
		g_assert_cmpuint(clawt_mailbox_item_get_round(item), ==, 0);
		g_assert_null(clawt_mailbox_item_get_chain(item));
	}

	clawt_test_remove_tree(dir);
}

/* rooms.max_chain: the schema default for a room that says nothing. */
static void
test_max_chain_defaults_from_the_schema(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(ClawtConfig) config = NULL;
	g_autoptr(GPtrArray) specs = NULL;
	g_autoptr(ClawtRoom) adhoc = clawt_room_new("adhoc", NULL);
	guint i;

	config = clawt_config_load_from_string(
		"rooms:\n"
		"  - id: quiet\n    members: [a, b, c]\n"
		"  - id: chatty\n    members: [a, b, c]\n    max_chain: 6\n"
		"  - id: uncapped\n    members: [a, b, c]\n    max_chain: 0\n",
		&error);
	g_assert_no_error(error);

	specs = clawt_config_get_rooms(config);
	g_assert_cmpuint(specs->len, ==, 3);

	for (i = 0; i < specs->len; i++) {
		ClawtRoomSpec *spec = g_ptr_array_index(specs, i);

		if (g_strcmp0(spec->id, "quiet") == 0)
			g_assert_cmpuint(spec->max_chain, ==, 3);
		else if (g_strcmp0(spec->id, "chatty") == 0)
			g_assert_cmpuint(spec->max_chain, ==, 6);
		else
			g_assert_cmpuint(spec->max_chain, ==, 0);
	}

	/* And a room no spec describes -- a direct room, one an agent made. */
	g_assert_cmpuint(clawt_room_get_max_chain(adhoc), ==, 3);
}

/* ── The tree budget, in the guard ───────────────────────────────── */

/* A resolver over a fixed table: task id -> root. */
static gchar *
table_tree(const gchar *sender_id, const gchar *room_id, const gchar *task_id,
           gpointer user_data)
{
	GHashTable *roots = user_data;

	(void)sender_id;
	(void)room_id;

	return (task_id != NULL)
		? g_strdup(g_hash_table_lookup(roots, task_id)) : NULL;
}

static gboolean
everyone_is_an_agent(const gchar *sender_id, gpointer user_data)
{
	(void)user_data;

	return g_strcmp0(sender_id, "user") != 0 &&
	       g_strcmp0(sender_id, CLAWT_SYSTEM_SENDER) != 0;
}

typedef struct {
	guint  alerts;
	gchar *root;
	guint  count;
	guint  limit;
} AlertSeen;

static void
on_tree_exhausted(ClawtLoopGuard *guard, const gchar *root, guint count,
                  guint limit, gpointer user_data)
{
	AlertSeen *seen = user_data;

	(void)guard;

	seen->alerts++;
	g_free(seen->root);
	seen->root = g_strdup(root);
	seen->count = count;
	seen->limit = limit;
}

static gboolean
request(ClawtLoopGuard *guard, const gchar *from, const gchar *task,
        GError **error)
{
	g_autoptr(ClawtMessage) message = clawt_message_new("room", from,
	                                                    "can you look?");

	clawt_message_set_task_id(message, task);

	return clawt_loop_guard_check_in_room(guard, message, 0, TRUE, error);
}

/*
 * Two trees, one budget each.  A shared counter would have tree A's
 * requests spend tree B's allowance; renewing one must not renew the
 * other; and the alert fires once per exhaustion, naming the tree.
 */
static void
test_tree_budget_is_per_tree(void)
{
	g_autoptr(ClawtLoopGuard) guard = clawt_loop_guard_new();
	g_autoptr(GHashTable) roots = g_hash_table_new(g_str_hash, g_str_equal);
	AlertSeen seen = { 0 };
	guint i;

	g_hash_table_insert(roots, "a-root", "a-root");
	g_hash_table_insert(roots, "a-child", "a-root");
	g_hash_table_insert(roots, "b-root", "b-root");

	clawt_loop_guard_set_limits(guard, 0, 0, 0);
	clawt_loop_guard_set_task_budget(guard, 0.0);
	clawt_loop_guard_set_tree_budget(guard, 3);
	clawt_loop_guard_set_peer_func(guard, everyone_is_an_agent, NULL, NULL);
	clawt_loop_guard_set_tree_func(guard, table_tree, roots, NULL);
	g_signal_connect(guard, "tree-exhausted",
	                 G_CALLBACK(on_tree_exhausted), &seen);

	/* Three from tree A, from two of its tasks. */
	g_assert_true(request(guard, "w1", "a-root", NULL));
	g_assert_true(request(guard, "w2", "a-child", NULL));
	g_assert_cmpuint(seen.alerts, ==, 0);
	g_assert_true(request(guard, "w2", "a-child", NULL));
	g_assert_cmpuint(seen.alerts, ==, 1);
	g_assert_cmpstr(seen.root, ==, "a-root");
	g_assert_cmpuint(seen.count, ==, 3);
	g_assert_cmpuint(seen.limit, ==, 3);
	g_assert_true(clawt_loop_guard_tree_exhausted(guard, "a-root"));

	/* The fourth is refused, saying what to do instead. */
	{
		g_autoptr(GError) error = NULL;

		g_assert_false(request(guard, "w1", "a-child", &error));
		g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT);
		g_assert_nonnull(strstr(error->message, "renews"));
	}
	g_assert_cmpuint(seen.alerts, ==, 1);

	/* Tree B is untouched by all of that. */
	g_assert_false(clawt_loop_guard_tree_exhausted(guard, "b-root"));
	for (i = 0; i < 3; i++)
		g_assert_true(request(guard, "w3", "b-root", NULL));
	g_assert_cmpuint(seen.alerts, ==, 2);
	g_assert_cmpstr(seen.root, ==, "b-root");

	/* Renewing A renews A only, and A may alert again later. */
	clawt_loop_guard_renew_tree(guard, "a-root");
	g_assert_false(clawt_loop_guard_tree_exhausted(guard, "a-root"));
	g_assert_true(clawt_loop_guard_tree_exhausted(guard, "b-root"));
	for (i = 0; i < 3; i++)
		g_assert_true(request(guard, "w1", "a-root", NULL));
	g_assert_cmpuint(seen.alerts, ==, 3);

	/* A message in no tree is never counted. */
	g_assert_true(request(guard, "w9", NULL, NULL));

	/* 0 is off. */
	clawt_loop_guard_set_tree_budget(guard, 0);
	g_assert_false(clawt_loop_guard_tree_exhausted(guard, "a-root"));
	g_assert_true(request(guard, "w1", "a-root", NULL));

	g_free(seen.root);
	clawt_loop_guard_set_tree_func(guard, NULL, NULL, NULL);
	clawt_loop_guard_set_peer_func(guard, NULL, NULL, NULL);
}

/*
 * Answers and the daemon's notices are never counted and never refused,
 * even with the tree at its limit: the delegator's owed follow-up starts
 * with a notice, and a finished worker's answer is what renews nothing
 * but strands everyone if it is refused.
 */
static void
test_answers_and_notices_are_never_refused(void)
{
	g_autoptr(ClawtLoopGuard) guard = clawt_loop_guard_new();
	g_autoptr(GHashTable) roots = g_hash_table_new(g_str_hash, g_str_equal);
	g_autoptr(ClawtMessage) answer = clawt_message_new("room", "w1", "done");
	g_autoptr(ClawtMessage) notice = clawt_message_new("room",
		CLAWT_SYSTEM_SENDER, "[clawtilla] t is finished");
	g_autoptr(ClawtMessage) fyi = clawt_message_new("room", "w1", "deployed");
	g_autoptr(GError) error = NULL;

	g_hash_table_insert(roots, "t", "t");
	clawt_loop_guard_set_limits(guard, 0, 0, 0);
	clawt_loop_guard_set_task_budget(guard, 0.0);
	clawt_loop_guard_set_tree_budget(guard, 1);
	clawt_loop_guard_set_peer_func(guard, everyone_is_an_agent, NULL, NULL);
	clawt_loop_guard_set_tree_func(guard, table_tree, roots, NULL);

	g_assert_true(request(guard, "w1", "t", NULL));
	g_assert_true(clawt_loop_guard_tree_exhausted(guard, "t"));

	clawt_message_set_task_id(answer, "t");
	clawt_message_set_invites_reply(answer, FALSE);
	g_assert_true(clawt_loop_guard_check_in_room(guard, answer, 0, TRUE,
	                                             &error));
	g_assert_no_error(error);

	clawt_message_set_task_id(notice, "t");
	clawt_message_set_invites_reply(notice, FALSE);
	g_assert_true(clawt_loop_guard_check_in_room(guard, notice, 0, TRUE,
	                                             &error));
	g_assert_no_error(error);
	g_assert_cmpuint(clawt_loop_guard_get_tree_count(guard, "t"), ==, 1);

	/* A one-way notice is still a request: it costs its reader a turn. */
	clawt_message_set_task_id(fyi, "t");
	clawt_message_set_one_way(fyi, TRUE);
	g_assert_false(clawt_loop_guard_check_in_room(guard, fyi, 0, TRUE,
	                                              &error));
	g_clear_error(&error);

	clawt_loop_guard_set_tree_func(guard, NULL, NULL, NULL);
	clawt_loop_guard_set_peer_func(guard, NULL, NULL, NULL);
}

/* ── The tree budget, through the daemon ─────────────────────────── */

typedef struct {
	gchar        *dir;
	gchar        *config_path;
	ClawtDaemon  *daemon;
	GMainContext *context;
} DaemonFixture;

static void
daemon_setup(DaemonFixture *fixture, const gchar *extra_yaml)
{
	g_autofree gchar *yaml = NULL;
	g_autoptr(GError) error = NULL;

	fixture->dir = g_dir_make_tmp("clawt-rounds-XXXXXX", NULL);
	fixture->config_path = g_build_filename(fixture->dir, "config.yaml",
	                                        NULL);

	/* Every path a daemon writes for itself, pinned inside the fixture. */
	yaml = g_strdup_printf(
		"daemon:\n"
		"  tailscale: false\n"
		"  state_dir: \"%s/state\"\n"
		"  socket: \"%s/daemon.sock\"\n"
		"  automation_dir: \"%s/pods\"\n"
		"skills:\n  dir: \"%s/skills\"\n"
		"%s"
		"agents:\n  - id: chief\n  - id: w1\n  - id: w2\n  - id: w3\n"
		"defaults:\n  workspace_root: \"%s/agents\"\n",
		fixture->dir, fixture->dir, fixture->dir, fixture->dir,
		extra_yaml != NULL ? extra_yaml : "", fixture->dir);
	g_file_set_contents(fixture->config_path, yaml, -1, &error);
	g_assert_no_error(error);

	fixture->context = g_main_context_new();
	fixture->daemon = clawt_daemon_new(fixture->config_path,
	                                   fixture->context);
	{
		gboolean started = clawt_daemon_start(fixture->daemon, &error);

		g_assert_no_error(error);
		g_assert_true(started);
	}
}

static void
daemon_teardown(DaemonFixture *fixture)
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

/* A request from @from carrying @task, through the daemon's router. */
static gint
daemon_request(DaemonFixture *fixture, const gchar *from, const gchar *to,
               const gchar *task, GError **error)
{
	g_autoptr(ClawtMessage) message = clawt_message_new(to, from,
	                                                    "can you check?");

	clawt_message_set_task_id(message, task);

	return clawt_mailbox_router_send(fixture->daemon->router, message,
	                                 error);
}

static guint
count_events(DaemonFixture *fixture, const gchar *kind, ClawtEvent **last)
{
	g_autoptr(GPtrArray) events = clawt_event_bus_replay(
		clawt_daemon_get_event_bus(fixture->daemon), 0, NULL);
	guint found = 0;
	guint i;

	for (i = 0; i < events->len; i++) {
		ClawtEvent *event = g_ptr_array_index(events, i);

		if (g_strcmp0(clawt_event_get_kind(event), kind) != 0)
			continue;

		found++;
		if (last != NULL) {
			g_clear_pointer(last, clawt_event_free);
			*last = clawt_event_copy(event);
		}
	}

	return found;
}

/*
 * The whole wire: the daemon resolves a delegated task's tree up its
 * parents, counts one request per call however many members it reaches,
 * withholds the messaging tools from a turn in the exhausted tree,
 * raises one alert naming it, and renews the tree when a task in it
 * settles -- before the settle notice, so the owed follow-up is never
 * refused.  Two trees throughout.
 */
static void
test_the_daemon_counts_per_tree_and_renews_on_settle(void)
{
	DaemonFixture fixture = { 0 };
	ClawtTaskManager *tasks;
	ClawtTask *root_a;
	ClawtTask *child_a;
	ClawtTask *root_b;
	ClawtAgent *w1;
	g_autoptr(ClawtEvent) alert = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *id_a = NULL;
	g_autofree gchar *id_child = NULL;
	g_autofree gchar *id_b = NULL;
	g_autofree gchar *dm = NULL;

	daemon_setup(&fixture,
		"orchestration:\n  tree_message_budget: 2\n"
		"rooms:\n  - id: crew\n    members: [w1, w2, w3]\n");
	tasks = clawt_daemon_get_tasks(fixture.daemon);

	root_a = clawt_task_manager_create(tasks, "chief", "w1",
		"Write the quarterly report", NULL, &error);
	g_assert_no_error(error);
	id_a = g_strdup(clawt_task_get_id(root_a));
	child_a = clawt_task_manager_create(tasks, "w1", "w2",
		"Pull the numbers", id_a, &error);
	g_assert_no_error(error);
	id_child = g_strdup(clawt_task_get_id(child_a));
	root_b = clawt_task_manager_create(tasks, "chief", "w3",
		"Tidy the wiki", NULL, &error);
	g_assert_no_error(error);
	id_b = g_strdup(clawt_task_get_id(root_b));
	clawt_task_manager_start(tasks, id_a);
	clawt_task_manager_start(tasks, id_child);
	clawt_task_manager_start(tasks, id_b);

	/* One post reaching two members of the crew is one request. */
	{
		g_autoptr(ClawtMessage) post = clawt_message_new("crew", "w1",
			"@w2 @w3 who has the Q3 sheet?");

		clawt_message_set_task_id(post, id_child);
		g_assert_cmpint(clawt_mailbox_router_send(fixture.daemon->router,
		                                          post, &error), ==, 2);
		g_assert_no_error(error);
	}
	g_assert_cmpuint(clawt_loop_guard_get_tree_count(fixture.daemon->guard,
	                                                 id_a), ==, 1);

	/* The second request, from the child task, reaches the limit. */
	g_assert_cmpint(daemon_request(&fixture, "w2", "w1", id_child, &error),
	                ==, 1);
	g_assert_no_error(error);
	g_assert_true(clawt_loop_guard_tree_exhausted(fixture.daemon->guard,
	                                              id_a));
	g_assert_cmpuint(count_events(&fixture, "task.tree_budget", &alert),
	                 ==, 1);
	g_assert_cmpstr(clawt_event_get_subject(alert), ==, id_a);
	g_assert_nonnull(strstr(clawt_event_get_detail(alert, "tree"),
	                        "Write the quarterly report"));
	g_assert_nonnull(strstr(clawt_event_get_detail(alert, "message"),
	                        "Write the quarterly report"));
	g_assert_cmpint(clawt_alert_tier_for_event(alert), ==,
	                CLAWT_ALERT_NOTICE);

	/* The third is refused, and tree B still has its whole budget. */
	g_assert_cmpint(daemon_request(&fixture, "w1", "w2", id_a, &error),
	                ==, -1);
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT);
	g_clear_error(&error);
	g_assert_cmpint(daemon_request(&fixture, "w3", "w1", id_b, &error),
	                ==, 1);
	g_assert_no_error(error);
	g_assert_cmpuint(count_events(&fixture, "task.tree_budget", NULL),
	                 ==, 1);

	/*
	 * The gate: w1's turn on its tree-A task is offered no messaging
	 * tool, the same tools max_hops withholds; reading the room's
	 * history is still offered.
	 */
	w1 = clawt_agent_manager_get(clawt_daemon_get_agents(fixture.daemon),
	                             "w1");
	dm = clawt_room_manager_direct_id("chief", "w1");
	clawt_agent_deliver_turn(w1, dm, 1, TRUE, "chief", id_a);
	clawt_agent_note_typing(w1, dm, TRUE);
	g_assert_false(clawt_mcp_tools_is_permitted(fixture.daemon->mcp_tools,
		"w1", "clawtilla_message_agent"));
	g_assert_true(clawt_mcp_tools_is_permitted(fixture.daemon->mcp_tools,
		"w1", "clawtilla_room_history"));

	/*
	 * The child settles.  The tree renews, the settle notice reaches
	 * the delegator -- never refused -- and w1 has its tools back.
	 */
	g_assert_true(clawt_task_manager_complete(tasks, id_child,
	                                          "Q3 numbers attached"));
	g_assert_cmpuint(clawt_loop_guard_get_tree_count(fixture.daemon->guard,
	                                                 id_a), ==, 0);
	g_assert_true(clawt_mcp_tools_is_permitted(fixture.daemon->mcp_tools,
		"w1", "clawtilla_message_agent"));
	g_assert_cmpuint(count_events(&fixture, "message.refused", NULL),
	                 ==, 1);
	g_assert_cmpint(daemon_request(&fixture, "w1", "w2", id_a, &error),
	                ==, 1);
	g_assert_no_error(error);

	clawt_agent_note_typing(w1, dm, FALSE);
	daemon_teardown(&fixture);
}

/*
 * What a turn on the settle notice would do, done at the moment the
 * notice is published: the delegator sends a request in the tree.
 */
typedef struct {
	DaemonFixture *fixture;
	const gchar   *child;
	const gchar   *root;
	gboolean       armed;
	gint           sent;
	gint           code;
} OwedFollowUp;

static void
on_notice_published(ClawtEventBus *bus, ClawtEvent *event,
                    gpointer user_data)
{
	OwedFollowUp *owed = user_data;
	g_autoptr(GError) error = NULL;

	(void)bus;

	if (!owed->armed ||
	    g_strcmp0(clawt_event_get_kind(event), "message") != 0 ||
	    g_strcmp0(clawt_event_get_detail(event, "from"),
	              CLAWT_SYSTEM_SENDER) != 0 ||
	    g_strcmp0(clawt_event_get_detail(event, "task"), owed->child) != 0)
		return;

	owed->armed = FALSE;
	owed->sent = daemon_request(owed->fixture, "w1", "w2", owed->root,
	                            &error);
	owed->code = (error != NULL) ? error->code : 0;
}

/*
 * The order, not just the renewal.  The tree renews in on_task_changed()
 * *before* the settle notice goes out, so anything the notice sets off --
 * here, a request in the tree sent the instant the notice is published --
 * finds the budget already renewed.  Renewed after the notice, that
 * request would be refused with the tree still exhausted.
 */
static void
test_the_tree_renews_before_the_settle_notice(void)
{
	DaemonFixture fixture = { 0 };
	ClawtTaskManager *tasks;
	ClawtTask *root;
	ClawtTask *child;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *id_root = NULL;
	g_autofree gchar *id_child = NULL;
	OwedFollowUp owed = { 0 };

	daemon_setup(&fixture, "orchestration:\n  tree_message_budget: 1\n");
	tasks = clawt_daemon_get_tasks(fixture.daemon);

	root = clawt_task_manager_create(tasks, "chief", "w1",
		"Write the quarterly report", NULL, &error);
	g_assert_no_error(error);
	id_root = g_strdup(clawt_task_get_id(root));
	child = clawt_task_manager_create(tasks, "w1", "w2",
		"Pull the numbers", id_root, &error);
	g_assert_no_error(error);
	id_child = g_strdup(clawt_task_get_id(child));
	clawt_task_manager_start(tasks, id_root);
	clawt_task_manager_start(tasks, id_child);

	/* The one request the tree may send; the next would be refused. */
	g_assert_cmpint(daemon_request(&fixture, "w2", "w1", id_child, &error),
	                ==, 1);
	g_assert_no_error(error);
	g_assert_true(clawt_loop_guard_tree_exhausted(fixture.daemon->guard,
	                                              id_root));

	owed.fixture = &fixture;
	owed.child = id_child;
	owed.root = id_root;
	owed.armed = TRUE;
	owed.sent = -2;
	g_signal_connect(clawt_daemon_get_event_bus(fixture.daemon), "event",
	                 G_CALLBACK(on_notice_published), &owed);

	g_assert_true(clawt_task_manager_complete(tasks, id_child,
	                                          "Q3 numbers attached"));

	/* The notice was published, and what it set off was not refused. */
	g_assert_false(owed.armed);
	g_assert_cmpint(owed.code, ==, 0);
	g_assert_cmpint(owed.sent, ==, 1);

	g_signal_handlers_disconnect_by_data(
		clawt_daemon_get_event_bus(fixture.daemon), &owed);
	daemon_teardown(&fixture);
}

/* The settings, read and written as the clients do. */
static void
test_the_settings_frames(void)
{
	DaemonFixture fixture = { 0 };
	g_autoptr(JsonNode) reply = NULL;
	JsonObject *payload;
	JsonArray *rooms;
	ClawtRoom *crew;

	daemon_setup(&fixture,
		"rooms:\n  - id: crew\n    members: [w1, w2, w3]\n"
		"  - id: pair\n    members: [w1, w2]\n");

	/* The defaults, and only the group room is listed. */
	{
		g_autoptr(JsonNode) frame = clawt_ipc_request_new(
			"orchestration.get", "1");

		reply = clawt_daemon_handle_request(fixture.daemon, frame);
	}
	payload = clawt_ipc_frame_get_payload(reply);
	g_assert_cmpint(json_object_get_int_member(payload,
		"tree_message_budget"), ==, 60);
	rooms = json_object_get_array_member(payload, "rooms");
	g_assert_cmpuint(json_array_get_length(rooms), ==, 1);
	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(rooms, 0), "id"), ==, "crew");
	g_assert_cmpint(json_object_get_int_member(
		json_array_get_object_element(rooms, 0), "max_chain"), ==, 3);
	g_clear_pointer(&reply, json_node_unref);

	/* The budget alone, as an integer; the guard has it at once. */
	{
		g_autoptr(JsonNode) frame = clawt_ipc_request_new(
			"orchestration.set", "2");
		g_autoptr(JsonBuilder) builder = json_builder_new();

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "tree_message_budget");
		json_builder_add_int_value(builder, 25);
		json_builder_end_object(builder);
		clawt_ipc_frame_set_payload(frame, json_builder_get_root(builder));
		reply = clawt_daemon_handle_request(fixture.daemon, frame);
	}
	g_assert_false(clawt_ipc_frame_is_error(reply));
	g_assert_cmpuint(clawt_loop_guard_get_tree_budget(fixture.daemon->guard),
	                 ==, 25);
	g_clear_pointer(&reply, json_node_unref);

	/* Out of range is refused. */
	{
		g_autoptr(JsonNode) frame = clawt_ipc_request_new(
			"orchestration.set", "3");
		g_autoptr(JsonBuilder) builder = json_builder_new();

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "tree_message_budget");
		json_builder_add_int_value(builder, -4);
		json_builder_end_object(builder);
		clawt_ipc_frame_set_payload(frame, json_builder_get_root(builder));
		reply = clawt_daemon_handle_request(fixture.daemon, frame);
	}
	g_assert_true(clawt_ipc_frame_is_error(reply));
	g_clear_pointer(&reply, json_node_unref);

	/* A room's thread cap, through room.set, saved and applied. */
	{
		g_autoptr(JsonNode) frame = clawt_ipc_request_new("room.set", "4");
		g_autoptr(JsonBuilder) builder = json_builder_new();

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "room");
		json_builder_add_string_value(builder, "crew");
		json_builder_set_member_name(builder, "max_chain");
		json_builder_add_int_value(builder, 5);
		json_builder_end_object(builder);
		clawt_ipc_frame_set_payload(frame, json_builder_get_root(builder));
		reply = clawt_daemon_handle_request(fixture.daemon, frame);
	}
	g_assert_false(clawt_ipc_frame_is_error(reply));
	crew = clawt_room_manager_get(clawt_daemon_get_rooms(fixture.daemon),
	                              "crew");
	g_assert_cmpuint(clawt_room_get_max_chain(crew), ==, 5);
	{
		g_autofree gchar *saved = NULL;

		g_assert_true(g_file_get_contents(fixture.config_path, &saved,
		                                  NULL, NULL));
		g_assert_nonnull(strstr(saved, "max_chain: 5"));
		g_assert_nonnull(strstr(saved, "tree_message_budget: 25"));
	}

	daemon_teardown(&fixture);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);

	g_test_add_func("/room-rounds/chain-helpers", test_chain_helpers);
	g_test_add_func("/room-rounds/many-rounds-never-reach-max-hops",
	                test_many_rounds_never_reach_max_hops);
	g_test_add_func("/room-rounds/mention-in-own-thread-withheld",
	                test_a_mention_inside_its_own_thread_is_withheld);
	g_test_add_func("/room-rounds/each-round-resets-the-depth",
	                test_each_round_resets_the_depth);
	g_test_add_func("/room-rounds/max-hops-backstop",
	                test_max_hops_is_still_the_backstop);
	g_test_add_func("/room-rounds/round-survives-in-the-mailbox",
	                test_round_survives_in_the_mailbox);
	g_test_add_func("/room-rounds/max-chain-default",
	                test_max_chain_defaults_from_the_schema);
	g_test_add_func("/tree-budget/per-tree", test_tree_budget_is_per_tree);
	g_test_add_func("/tree-budget/answers-and-notices",
	                test_answers_and_notices_are_never_refused);
	g_test_add_func("/tree-budget/daemon",
	                test_the_daemon_counts_per_tree_and_renews_on_settle);
	g_test_add_func("/tree-budget/renews-before-the-notice",
	                test_the_tree_renews_before_the_settle_notice);
	g_test_add_func("/tree-budget/settings-frames", test_the_settings_frames);

	return g_test_run();
}
