/*
 * test-group-room.c - Rooms with more than two members
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A group room is the first room where more than one agent can be
 * mid-turn at once, and that is a shape none of the per-room state was
 * written for.  Every test here therefore uses **two** agents in **one**
 * room: a fixture with one of each cannot see a shared-scalar bug, and
 * each of the mechanisms below held perfectly until a second holder
 * existed.
 *
 * The daemon half includes core/clawt-daemon-private.h directly, for the
 * reason test-turn-hygiene.c already records: that header *is* the
 * interface of src/core/daemon-turn.c, and settling a turn is something
 * libreclaw's typing frame does, which a hermetic test has no way to
 * reach through the IPC surface.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>
#include <string.h>
#include <sys/socket.h>

#include "clawt-test-util.h"

#include "core/clawt-daemon-private.h"

typedef struct {
    gchar        *dir;
    gchar        *config_path;
    ClawtDaemon  *daemon;
    GMainContext *context;
} Fixture;

static void
fixture_setup(Fixture *fixture, const gchar *extra_yaml)
{
    g_autofree gchar *yaml = NULL;
    g_autoptr(GError) error = NULL;

    fixture->dir = g_dir_make_tmp("clawt-group-XXXXXX", NULL);
    fixture->config_path = g_build_filename(fixture->dir, "config.yaml",
                                            NULL);

    /*
     * Five things pinned, every one of which otherwise escapes into the
     * developer's own fleet or onto the network.
     */
    yaml = g_strdup_printf(
        "daemon:\n"
        "  tailscale: false\n"
        "  state_dir: \"%s/state\"\n"
        "  socket: \"%s/daemon.sock\"\n"
        "  automation_dir: \"%s/pods\"\n"
        "defaults:\n  workspace_root: \"%s/agents\"\n"
        "%s",
        fixture->dir, fixture->dir, fixture->dir, fixture->dir,
        extra_yaml != NULL ? extra_yaml : "");

    g_file_set_contents(fixture->config_path, yaml, -1, &error);
    g_assert_no_error(error);

    fixture->context = g_main_context_new();
    fixture->daemon = clawt_daemon_new(fixture->config_path,
                                       fixture->context);

    g_assert_true(clawt_daemon_start(fixture->daemon, &error));
    g_assert_no_error(error);
}

static void
fixture_teardown(Fixture *fixture)
{
    if (fixture->daemon != NULL) {
        clawt_daemon_stop(fixture->daemon);
        g_clear_object(&fixture->daemon);
    }

    if (fixture->context != NULL) {
        while (g_main_context_iteration(fixture->context, FALSE))
            ;
    }

    g_clear_pointer(&fixture->context, g_main_context_unref);

    if (fixture->dir != NULL)
        clawt_test_remove_tree(fixture->dir);

    g_clear_pointer(&fixture->dir, g_free);
    g_clear_pointer(&fixture->config_path, g_free);
}

/*
 * A room with three members and a budget, which is the only shape in
 * which two agents can hold one room's turn at the same time.
 */
static const gchar *STANDUP_YAML =
    "rooms:\n"
    "  - id: standup\n"
    "    members: [alice, bob, carol]\n"
    "    turn_timeout_seconds: 60\n";

/*
 * How many of the room's holders are this agent.
 *
 * By value rather than by key, so the test says nothing about how a
 * hold is spelled -- the point being asserted is that two of them can
 * exist at once, not what the hash table looks like.
 */
static guint
holds_by(ClawtDaemon *daemon, const gchar *agent_id)
{
    GHashTableIter iter;
    gpointer key;
    gpointer value;
    guint found = 0;

    g_hash_table_iter_init(&iter, daemon->room_holder);

    while (g_hash_table_iter_next(&iter, &key, &value)) {
        if (g_strcmp0(value, agent_id) == 0)
            found++;
    }

    return found;
}

/*
 * Two members mid-turn in one room are two holders, not one.
 *
 * `room_holder` was `room id -> agent id`, a scalar per room, which is
 * exactly right while a room has two members and one of them is the
 * operator.  Mention two agents in a group and both take a turn: the
 * second overwrote the first, so `rooms.turn_timeout_seconds` could
 * only ever fire naming whichever had started last, and the one it
 * named might have finished long ago.
 */
static void
test_two_members_hold_one_room_at_once(void)
{
    Fixture fixture = { 0 };

    fixture_setup(&fixture, STANDUP_YAML);

    clawt_daemon_turn_begin_room(fixture.daemon, "alice", "standup");
    clawt_daemon_turn_begin_room(fixture.daemon, "bob", "standup");

    g_assert_cmpuint(holds_by(fixture.daemon, "alice"), ==, 1);
    g_assert_cmpuint(holds_by(fixture.daemon, "bob"), ==, 1);

    fixture_teardown(&fixture);
}

/*
 * And one of them finishing does not end the other's turn.
 *
 * The settle took only the room, so either member's falling edge
 * removed the single holder and ended the watch -- leaving the member
 * still working unwatched, and the budget unenforceable against the
 * one it was written for.
 */
static void
test_one_member_settling_leaves_the_other_holding(void)
{
    Fixture fixture = { 0 };

    fixture_setup(&fixture, STANDUP_YAML);

    clawt_daemon_turn_begin_room(fixture.daemon, "alice", "standup");
    clawt_daemon_turn_begin_room(fixture.daemon, "bob", "standup");

    clawt_daemon_turn_settle_room(fixture.daemon, "alice", "standup");

    g_assert_cmpuint(holds_by(fixture.daemon, "alice"), ==, 0);
    g_assert_cmpuint(holds_by(fixture.daemon, "bob"), ==, 1);

    clawt_daemon_turn_settle_room(fixture.daemon, "bob", "standup");

    g_assert_cmpuint(holds_by(fixture.daemon, "bob"), ==, 0);

    fixture_teardown(&fixture);
}

/*
 * A member holding two rooms still releases both when it stops.
 *
 * The regression guard for the composite key: an agent's own settle
 * finds its holds by value, and must keep finding all of them.
 */
static void
test_an_agent_settling_releases_every_room_it_held(void)
{
    Fixture fixture = { 0 };

    fixture_setup(&fixture,
                  "rooms:\n"
                  "  - id: standup\n"
                  "    members: [alice, bob, carol]\n"
                  "    turn_timeout_seconds: 60\n"
                  "  - id: design\n"
                  "    members: [alice, bob]\n"
                  "    turn_timeout_seconds: 60\n");

    clawt_daemon_turn_begin_room(fixture.daemon, "alice", "standup");
    clawt_daemon_turn_begin_room(fixture.daemon, "alice", "design");
    clawt_daemon_turn_begin_room(fixture.daemon, "bob", "standup");

    g_assert_cmpuint(holds_by(fixture.daemon, "alice"), ==, 2);

    clawt_daemon_turn_settle(fixture.daemon, "alice");

    g_assert_cmpuint(holds_by(fixture.daemon, "alice"), ==, 0);
    g_assert_cmpuint(holds_by(fixture.daemon, "bob"), ==, 1);

    fixture_teardown(&fixture);
}


/* ── The router, the mention rule and the loop guard ─────────────── */

typedef struct {
    gchar              *dir;
    ClawtConfig        *config;
    ClawtAgentManager  *agents;
    ClawtRoomManager   *rooms;
    ClawtLoopGuard     *guard;
    ClawtMailboxRouter *router;
    GSocket            *far_end;
} RouterFixture;

/*
 * Which senders are agents, which is what lets the guard end an
 * exchange rather than only refuse another message.  Without it nothing
 * ever stalls and the standup test below would pass against a build
 * where the bug was still there.
 */
static gboolean
sender_is_an_agent(const gchar *sender, gpointer user_data)
{
    RouterFixture *fixture = user_data;

    return sender != NULL &&
           clawt_agent_manager_get(fixture->agents, sender) != NULL;
}

static void
router_setup(RouterFixture *fixture)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *yaml = NULL;

    fixture->dir = g_dir_make_tmp("clawt-groupr-XXXXXX", NULL);

    yaml = g_strdup_printf(
        "daemon:\n"
        "  state_dir: \"%s\"\n"
        "  socket: \"%s/daemon.sock\"\n"
        "defaults:\n  workspace_root: \"%s/agents\"\n"
        "agents:\n"
        "  - id: alice\n    name: Alice\n"
        "  - id: bob\n    name: Bob\n"
        "  - id: carol\n    name: Carol\n",
        fixture->dir, fixture->dir, fixture->dir);

    fixture->config = clawt_config_load_from_string(yaml, &error);
    g_assert_no_error(error);

    fixture->agents = clawt_agent_manager_new(fixture->config);
    clawt_agent_manager_load(fixture->agents, NULL);

    fixture->rooms = clawt_room_manager_new(NULL);

    fixture->guard = clawt_loop_guard_new();
    clawt_loop_guard_set_limits(fixture->guard, 8, 30, 10);
    clawt_loop_guard_set_cycle_seconds(fixture->guard, 300);
    clawt_loop_guard_set_peer_func(fixture->guard, sender_is_an_agent,
                                   fixture, NULL);

    fixture->router = clawt_mailbox_router_new(fixture->agents,
                                               fixture->rooms,
                                               fixture->guard);
}

static void
router_teardown(RouterFixture *fixture)
{
    g_clear_object(&fixture->far_end);
    g_clear_object(&fixture->router);
    g_clear_object(&fixture->guard);
    g_clear_object(&fixture->rooms);
    g_clear_object(&fixture->agents);
    g_clear_object(&fixture->config);

    if (fixture->dir != NULL)
        clawt_test_remove_tree(fixture->dir);

    g_clear_pointer(&fixture->dir, g_free);
}

/* A three-member room that only delivers what names somebody. */
static ClawtRoom *
mention_room(RouterFixture *fixture)
{
    ClawtRoom *room = clawt_room_manager_create(fixture->rooms, "standup",
                                                "Standup", NULL);

    g_assert_nonnull(room);

    clawt_room_add_member(room, "alice");
    clawt_room_add_member(room, "bob");
    clawt_room_add_member(room, "carol");
    clawt_room_set_require_mention(room, TRUE);

    return room;
}

static gint
post(RouterFixture *fixture, const gchar *from, const gchar *body)
{
    g_autoptr(ClawtMessage) message =
        clawt_message_new("standup", from, body);

    return clawt_mailbox_router_send(fixture->router, message, NULL);
}

/*
 * A remark that names nobody is recorded and reaches nobody.
 *
 * This is the whole feature in one assertion: being in the room is not
 * being asked a question, so five members do not each take a turn on
 * every line.
 */
static void
test_naming_nobody_reaches_nobody_and_is_still_recorded(void)
{
    RouterFixture fixture = { 0 };
    ClawtRoom *room;

    router_setup(&fixture);
    room = mention_room(&fixture);

    g_assert_cmpint(post(&fixture, "user", "morning all"), ==, 0);
    g_assert_cmpuint(clawt_room_get_message_count(room), ==, 1);

    router_teardown(&fixture);
}

/* And one that names somebody reaches exactly them. */
static void
test_naming_one_member_reaches_exactly_that_member(void)
{
    RouterFixture fixture = { 0 };

    router_setup(&fixture);
    mention_room(&fixture);

    g_assert_cmpint(post(&fixture, "user", "@alice what do you think?"),
                    ==, 1);
    g_assert_cmpint(post(&fixture, "user", "alice and @bob please"), ==, 2);

    router_teardown(&fixture);
}

/*
 * `@all` is the operator's, and an agent writing it reaches nobody.
 *
 * An agent that could broadcast would turn one reply into a turn for
 * every other member, each of which could broadcast again.
 */
static void
test_only_a_non_agent_sender_may_broadcast(void)
{
    RouterFixture fixture = { 0 };

    router_setup(&fixture);
    mention_room(&fixture);

    /* The operator reaches all three. */
    g_assert_cmpint(post(&fixture, "user", "@all standup now"), ==, 3);

    /* The daemon's own automation is the operator's, and reaches them. */
    g_assert_cmpint(post(&fixture, "routine", "@all nightly report"), ==, 3);

    /* An agent's does not. */
    g_assert_cmpint(post(&fixture, "alice", "@all I am done"), ==, 0);

    /* But naming somebody still works from an agent. */
    g_assert_cmpint(post(&fixture, "alice", "@all -- @bob especially"),
                    ==, 1);

    router_teardown(&fixture);
}

/*
 * Saying the same thing twice to nobody does not end the room.
 *
 * The cycle detector fingerprints sender, room and body, and for an
 * agent sender an exact repeat inside `cycle_seconds` does not refuse
 * -- it **stalls the room**, which takes a person to undo and stalls
 * every task its members hold.  Once a group records the closing text
 * of every turn, one member writing "Acknowledged." twice in five
 * minutes would have ended the standup.
 */
static void
test_a_repeat_that_reaches_nobody_does_not_stall_the_room(void)
{
    RouterFixture fixture = { 0 };

    router_setup(&fixture);
    mention_room(&fixture);

    g_assert_cmpint(post(&fixture, "alice", "Acknowledged."), ==, 0);
    g_assert_cmpint(post(&fixture, "alice", "Acknowledged."), ==, 0);

    g_assert_cmpint(clawt_loop_guard_get_stall_reason(fixture.guard,
                                                      "standup"),
                    ==, CLAWT_STALL_NONE);

    router_teardown(&fixture);
}

/*
 * And saying it twice to somebody still does.
 *
 * The guard is not being switched off in group rooms -- a genuine
 * mutual-mention ping-pong is exactly what it is for, and this is the
 * assertion that says the fix above narrowed the input rather than
 * removing the check.
 */
static void
test_a_repeat_that_reaches_somebody_still_stalls_the_room(void)
{
    RouterFixture fixture = { 0 };

    router_setup(&fixture);
    mention_room(&fixture);

    g_assert_cmpint(post(&fixture, "alice", "@bob same thing"), ==, 1);
    g_assert_cmpint(post(&fixture, "alice", "@bob same thing"), <, 0);

    g_assert_cmpint(clawt_loop_guard_get_stall_reason(fixture.guard,
                                                      "standup"),
                    ==, CLAWT_STALL_REPEATED_MESSAGE);

    router_teardown(&fixture);
}


/* ── What a member is told on every message ──────────────────────── */

/*
 * Gives @agent a link the drain will actually write into.
 *
 * A socketpair rather than the link server, because a drain with no
 * open link returns before it builds a preamble at all -- so a test
 * that skipped this would pass against a build where the text had never
 * been written.  The read side is never started; nothing here speaks
 * the agent's half of the protocol.
 */
static void
give_agent_a_link(RouterFixture *fixture, ClawtAgent *agent)
{
    g_autoptr(GSocket) near_end = NULL;
    g_autoptr(GSocketConnection) connection = NULL;
    g_autoptr(ClawtLink) link = NULL;
    g_autoptr(GError) error = NULL;
    int fds[2];

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), ==, 0);

    near_end = g_socket_new_from_fd(fds[0], &error);
    g_assert_no_error(error);

    fixture->far_end = g_socket_new_from_fd(fds[1], &error);
    g_assert_no_error(error);

    connection = g_socket_connection_factory_create_connection(near_end);
    link = clawt_link_new(connection);

    clawt_agent_set_link(agent, link);
}

/* What the agent was actually handed, as text. */
static gchar *
read_delivered(RouterFixture *fixture)
{
    gchar buffer[16384];
    gssize got;
    g_autoptr(GError) error = NULL;

    g_socket_set_blocking(fixture->far_end, FALSE);
    got = g_socket_receive(fixture->far_end, buffer, sizeof(buffer) - 1,
                           NULL, &error);

    if (got <= 0)
        return NULL;

    buffer[got] = '\0';
    return g_strdup(buffer);
}

/*
 * Every message into a group carries the roster and the addressing
 * rule -- and the permission to answer without naming anybody.
 *
 * The last clause is asserted specifically rather than by checking that
 * a preamble exists, because it is the one sentence keeping the room
 * quiet.  An AI CLI cannot end a turn without writing something, so a
 * preamble that read as "name somebody" would produce an agent that
 * names somebody every time -- the runaway the mention rule exists to
 * prevent, rebuilt out of prompt text.  A test that only looked for the
 * roster would pass against a build that had lost it.
 */
static void
test_a_group_delivery_names_the_room_and_permits_silence(void)
{
    RouterFixture fixture = { 0 };
    ClawtAgent *bob;
    g_autofree gchar *delivered = NULL;

    router_setup(&fixture);
    mention_room(&fixture);

    bob = clawt_agent_manager_get(fixture.agents, "bob");
    g_assert_nonnull(bob);
    give_agent_a_link(&fixture, bob);

    g_assert_cmpint(post(&fixture, "user", "@bob what do you think?"),
                    ==, 1);

    delivered = read_delivered(&fixture);
    g_assert_nonnull(delivered);

    /* Which room, and that it is a room rather than the operator. */
    g_assert_nonnull(strstr(delivered, "room 'standup'"));

    /* Who else is in it, by the name the matcher accepts. */
    g_assert_nonnull(strstr(delivered, "@alice"));
    g_assert_nonnull(strstr(delivered, "@carol"));

    /* And not itself: an agent cannot address its own reply to itself. */
    g_assert_null(strstr(delivered, "@bob (Bob)"));

    /* How to reach one of them. */
    g_assert_nonnull(strstr(delivered, "@name"));

    /* The clause that keeps the room quiet. */
    g_assert_nonnull(strstr(delivered,
                            "Most replies here should name nobody"));

    /* And that broadcasting is not theirs to do. */
    g_assert_nonnull(strstr(delivered, "@all"));

    /*
     * And which conversation to name back, which a group needs whoever
     * sent the message: being in a room is the case where an agent has
     * several going at once.
     */
    g_assert_nonnull(strstr(delivered, "turn_room"));

    /* The message itself is still there, after all of that. */
    g_assert_nonnull(strstr(delivered, "what do you think?"));

    router_teardown(&fixture);
}

/*
 * A roster is not stale, because it is built per delivery.
 *
 * The alternative was a managed region in the agent's workspace, which
 * is refreshed at agent start -- so a member added to a group today
 * would learn about it after its next restart, and a removed one would
 * go on being told it is still a member.
 */
static void
test_the_roster_follows_a_membership_change(void)
{
    RouterFixture fixture = { 0 };
    ClawtRoom *room;
    ClawtAgent *bob;
    g_autofree gchar *first = NULL;
    g_autofree gchar *second = NULL;

    router_setup(&fixture);
    room = mention_room(&fixture);

    bob = clawt_agent_manager_get(fixture.agents, "bob");
    give_agent_a_link(&fixture, bob);

    g_assert_cmpint(post(&fixture, "user", "@bob one"), ==, 1);
    first = read_delivered(&fixture);
    g_assert_nonnull(strstr(first, "@carol"));

    g_assert_true(clawt_room_remove_member(room, "carol"));

    g_assert_cmpint(post(&fixture, "user", "@bob two"), ==, 1);
    second = read_delivered(&fixture);
    g_assert_nonnull(second);
    g_assert_null(strstr(second, "@carol"));
    g_assert_nonnull(strstr(second, "@alice"));

    router_teardown(&fixture);
}

/*
 * And a pair still gets the pair text.
 *
 * The regression guard: the group preamble is chosen by the room's
 * mention rule, so a two-member conversation must be untouched in both
 * directions.
 */
static void
test_a_pair_still_gets_the_pair_preamble(void)
{
    RouterFixture fixture = { 0 };
    ClawtRoom *room;
    ClawtAgent *bob;
    g_autofree gchar *delivered = NULL;

    router_setup(&fixture);

    room = clawt_room_manager_create(fixture.rooms, "pair", NULL, NULL);
    g_assert_nonnull(room);
    clawt_room_add_member(room, "alice");
    clawt_room_add_member(room, "bob");

    bob = clawt_agent_manager_get(fixture.agents, "bob");
    give_agent_a_link(&fixture, bob);

    {
        g_autoptr(ClawtMessage) message =
            clawt_message_new("pair", "alice", "a question for you");

        g_assert_cmpint(clawt_mailbox_router_send(fixture.router, message,
                                                  NULL), ==, 1);
    }

    delivered = read_delivered(&fixture);
    g_assert_nonnull(delivered);

    g_assert_nonnull(strstr(delivered, "another agent in your fleet"));
    g_assert_null(strstr(delivered, "Most replies here should name nobody"));

    router_teardown(&fixture);
}


/* ── How many context windows a member is ────────────────────────── */

/*
 * A group member defaults to `room` mode, and an orchestrator still
 * takes `agent`.
 *
 * Not a preference.  Under `sender-room` an agent has a session per
 * speaker *in the same room*, and every piece of the daemon's per-room
 * turn state is keyed on the room alone -- the typing indicator carries
 * the room and not the session, so two such turns cannot be told apart
 * at all.
 */
static void
test_a_group_member_is_one_conversation_per_room(void)
{
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(GError) error = NULL;
    ClawtAgentConfig *alice;
    ClawtAgentConfig *dave;
    ClawtAgentConfig *chief;

    config = clawt_config_load_from_string(
        "agents:\n"
        "  - id: alice\n"
        "  - id: bob\n"
        "  - id: carol\n"
        "  - id: dave\n"
        "  - id: chief\n    chief_of_staff: true\n"
        "rooms:\n"
        "  - id: standup\n"
        "    members: [alice, bob, carol, chief]\n"
        "  - id: pair\n"
        "    members: [dave, bob]\n",
        &error);
    g_assert_no_error(error);

    alice = clawt_config_get_agent(config, "alice");
    dave = clawt_config_get_agent(config, "dave");
    chief = clawt_config_get_agent(config, "chief");

    g_assert_cmpstr(clawt_agent_config_get_string(alice,
                                                  "session.routing_mode"),
                    ==, "room");

    /* A two-member room is a conversation, and keeps the ordinary default. */
    g_assert_cmpstr(clawt_agent_config_get_string(dave,
                                                  "session.routing_mode"),
                    ==, "sender-room");

    /* And the stronger answer wins for an orchestrator that is also here. */
    g_assert_cmpstr(clawt_agent_config_get_string(chief,
                                                  "session.routing_mode"),
                    ==, "agent");
}

/*
 * And an explicit value still wins, except the one that cannot work.
 */
static void
test_an_explicit_routing_mode_wins(void)
{
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(GError) error = NULL;

    config = clawt_config_load_from_string(
        "agents:\n"
        "  - id: alice\n    session:\n      routing_mode: agent\n"
        "  - id: bob\n"
        "  - id: carol\n"
        "rooms:\n"
        "  - id: standup\n"
        "    members: [alice, bob, carol]\n",
        &error);
    g_assert_no_error(error);

    g_assert_cmpstr(
        clawt_agent_config_get_string(clawt_config_get_agent(config, "alice"),
                                      "session.routing_mode"),
        ==, "agent");
}

/*
 * A room that says nothing about mentions follows its shape.
 *
 * The schema default is `false`, which is right for a conversation and
 * wrong for a standup: without a mention rule every member takes a turn
 * on every remark.  One resolver rather than a default here and a
 * different answer at each creation site.
 */
static void
test_the_mention_rule_follows_the_room_shape(void)
{
    g_autoptr(ClawtRoomManager) rooms = clawt_room_manager_new(NULL);
    ClawtRoom *pair = clawt_room_manager_create(rooms, "pair", NULL, NULL);
    ClawtRoom *group = clawt_room_manager_create(rooms, "group", NULL, NULL);

    clawt_room_add_member(pair, "alice");
    clawt_room_add_member(pair, "bob");

    clawt_room_add_member(group, "alice");
    clawt_room_add_member(group, "bob");
    clawt_room_add_member(group, "carol");

    g_assert_false(clawt_room_is_group(pair));
    g_assert_false(clawt_room_get_require_mention(pair));

    g_assert_true(clawt_room_is_group(group));
    g_assert_true(clawt_room_get_require_mention(group));

    /* And a room that was told keeps what it was told. */
    clawt_room_set_require_mention(group, FALSE);
    g_assert_false(clawt_room_get_require_mention(group));

    clawt_room_set_require_mention(pair, TRUE);
    g_assert_true(clawt_room_get_require_mention(pair));
}

int
main(int argc, char **argv)
{
    g_autofree gchar *data_dir = g_dir_make_tmp("clawt-group-data-XXXXXX",
                                                NULL);
    gint status;

    /*
     * Before anything can ask for it: GLib caches the data directory on
     * first use, so setting it after g_test_init() reaches nothing.
     */
    g_setenv("XDG_DATA_HOME", data_dir, TRUE);

    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/group/room/two-members-hold-at-once",
                    test_two_members_hold_one_room_at_once);
    g_test_add_func("/group/room/settling-one-leaves-the-other",
                    test_one_member_settling_leaves_the_other_holding);
    g_test_add_func("/group/room/agent-settle-releases-every-room",
                    test_an_agent_settling_releases_every_room_it_held);
    g_test_add_func("/group/mention/naming-nobody-reaches-nobody",
                    test_naming_nobody_reaches_nobody_and_is_still_recorded);
    g_test_add_func("/group/mention/naming-one-reaches-one",
                    test_naming_one_member_reaches_exactly_that_member);
    g_test_add_func("/group/mention/only-a-non-agent-may-broadcast",
                    test_only_a_non_agent_sender_may_broadcast);
    g_test_add_func("/group/guard/repeat-to-nobody-does-not-stall",
                    test_a_repeat_that_reaches_nobody_does_not_stall_the_room);
    g_test_add_func("/group/guard/repeat-to-somebody-still-stalls",
                    test_a_repeat_that_reaches_somebody_still_stalls_the_room);
    g_test_add_func("/group/preamble/names-the-room-and-permits-silence",
                    test_a_group_delivery_names_the_room_and_permits_silence);
    g_test_add_func("/group/preamble/roster-follows-membership",
                    test_the_roster_follows_a_membership_change);
    g_test_add_func("/group/preamble/a-pair-is-unchanged",
                    test_a_pair_still_gets_the_pair_preamble);
    g_test_add_func("/group/routing/member-is-one-conversation-per-room",
                    test_a_group_member_is_one_conversation_per_room);
    g_test_add_func("/group/routing/explicit-wins",
                    test_an_explicit_routing_mode_wins);
    g_test_add_func("/group/mention/rule-follows-the-room-shape",
                    test_the_mention_rule_follows_the_room_shape);

    status = g_test_run();

    clawt_test_remove_tree(data_dir);

    return status;
}
