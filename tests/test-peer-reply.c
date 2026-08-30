/*
 * test-peer-reply.c - What stops two agents answering each other for ever
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * An AI CLI cannot decline to produce text.  Whatever it writes at the
 * end of a turn is the reply, and clawtilla used to route every one of
 * them -- so an agent told "answer if you have something to say and end
 * your turn without replying if you do not" was being asked for
 * something it had no way to do, and a one-line greeting between two
 * agents ran until `orchestration.max_hops` cut it off eight turns
 * later.
 *
 * The rule these tests hold down is that a reply invites none of its
 * own: a message somebody chose to send earns exactly one answer, and
 * the exchange stops there.  Anything further has to be another
 * deliberate clawtilla_message_agent call, which is the difference
 * between a conversation and a loop.
 */

#include <clawtilla.h>

#include <gio/gio.h>
#include <glib/gstdio.h>

#include <sqlite3.h>
#include <sys/socket.h>

#include "clawt-test-util.h"

/* ── The flag itself ─────────────────────────────────────────────── */

/*
 * A message invites a reply unless somebody says otherwise.
 *
 * The default has to be this way round: everything that reaches a
 * mailbox other than an agent's own turn output was written on purpose,
 * and refusing to answer an operator would be a far worse failure than
 * one extra hop between two agents.
 */
static void
test_a_message_invites_a_reply_by_default(void)
{
    g_autoptr(ClawtMessage) message = clawt_message_new("room", "alpha",
                                                        "hello");
    g_autoptr(ClawtMessage) copy = NULL;

    g_assert_true(clawt_message_get_invites_reply(message));

    clawt_message_set_invites_reply(message, FALSE);
    g_assert_false(clawt_message_get_invites_reply(message));

    /*
     * And a copy carries it.  The router copies a message on its way to
     * the transcript, so a field the copy drops is a field that is true
     * of the original and of nothing downstream.
     */
    copy = clawt_message_copy(message);
    g_assert_false(clawt_message_get_invites_reply(copy));
}

static void
test_an_item_invites_a_reply_by_default(void)
{
    g_autoptr(ClawtMailboxItem) item = clawt_mailbox_item_new("a", "b", "hi");
    g_autoptr(ClawtMailboxItem) copy = NULL;

    g_assert_true(clawt_mailbox_item_get_invites_reply(item));

    clawt_mailbox_item_set_invites_reply(item, FALSE);
    g_assert_false(clawt_mailbox_item_get_invites_reply(item));

    copy = clawt_mailbox_item_copy(item);
    g_assert_false(clawt_mailbox_item_get_invites_reply(copy));
}

/* ── Storage ─────────────────────────────────────────────────────── */

/*
 * The flag outlives the daemon that queued the message.
 *
 * A mailbox is durable precisely so a stopped agent still gets its post,
 * and an item can sit in one across a restart.  Keeping this in memory
 * would mean a message queued before a restart was delivered as though
 * somebody had chosen to send it, and the exchange it closes would open
 * again.
 */
static void
test_the_flag_survives_a_restart(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-peer-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(dir, "mailbox.db", NULL);
    g_autofree gchar *quiet_id = NULL;
    g_autofree gchar *loud_id = NULL;

    {
        g_autoptr(ClawtMailbox) mailbox = NULL;
        g_autoptr(ClawtMailboxItem) quiet = NULL;
        g_autoptr(ClawtMailboxItem) loud = NULL;
        g_autoptr(GError) error = NULL;

        mailbox = clawt_mailbox_new("beta", path, &error);
        g_assert_no_error(error);

        quiet = clawt_mailbox_item_new("alpha", "beta", "that is all");
        clawt_mailbox_item_set_invites_reply(quiet, FALSE);
        quiet_id = clawt_mailbox_post(mailbox, quiet, &error);
        g_assert_no_error(error);

        loud = clawt_mailbox_item_new("alpha", "beta", "what do you think?");
        loud_id = clawt_mailbox_post(mailbox, loud, &error);
        g_assert_no_error(error);
    }

    {
        g_autoptr(ClawtMailbox) mailbox = NULL;
        g_autoptr(ClawtMailboxItem) quiet = NULL;
        g_autoptr(ClawtMailboxItem) loud = NULL;
        g_autoptr(GError) error = NULL;

        mailbox = clawt_mailbox_new("beta", path, &error);
        g_assert_no_error(error);

        quiet = clawt_mailbox_get(mailbox, quiet_id);
        g_assert_nonnull(quiet);
        g_assert_false(clawt_mailbox_item_get_invites_reply(quiet));

        loud = clawt_mailbox_get(mailbox, loud_id);
        g_assert_nonnull(loud);
        g_assert_true(clawt_mailbox_item_get_invites_reply(loud));
    }

    clawt_test_remove_tree(dir);
}

/*
 * A mailbox written by an older build still opens, and its items read as
 * inviting a reply.
 *
 * CREATE TABLE IF NOT EXISTS does nothing to a file that already has the
 * table, so a column added after the first release reaches a new
 * database and no existing one.  Without the migration every mailbox in
 * a live fleet would answer "no such column" on the first read and be
 * quarantined as corrupt -- a fleet's queued work moved aside on
 * upgrade.
 *
 * The old rows read TRUE because that is what they meant: they were
 * queued by a build in which every message earned an answer.
 */
static void
test_a_mailbox_from_an_older_build_migrates(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-peer-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(dir, "mailbox.db", NULL);
    g_autoptr(ClawtMailbox) mailbox = NULL;
    g_autoptr(ClawtMailboxItem) item = NULL;
    g_autoptr(GError) error = NULL;
    sqlite3 *db = NULL;

    /* The schema as it stood before invites_reply, with one item in it. */
    g_assert_cmpint(sqlite3_open(path, &db), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_exec(db,
        "CREATE TABLE items ("
        "  id TEXT PRIMARY KEY, sender TEXT, recipient TEXT,"
        "  body TEXT NOT NULL, room TEXT, task_id TEXT, reply_to TEXT,"
        "  subject TEXT, idempotency_key TEXT, last_error TEXT,"
        "  priority INTEGER NOT NULL DEFAULT 1,"
        "  state INTEGER NOT NULL DEFAULT 0,"
        "  depth INTEGER NOT NULL DEFAULT 0,"
        "  attempts INTEGER NOT NULL DEFAULT 0,"
        "  created_at INTEGER NOT NULL,"
        "  not_before INTEGER NOT NULL DEFAULT 0,"
        "  expires_at INTEGER NOT NULL DEFAULT 0,"
        "  lease_expires_at INTEGER NOT NULL DEFAULT 0);"
        "INSERT INTO items (id, sender, recipient, body, state, created_at)"
        "  VALUES ('old-1', 'alpha', 'beta', 'from before', 0, 1);",
        NULL, NULL, NULL), ==, SQLITE_OK);
    sqlite3_close_v2(db);

    mailbox = clawt_mailbox_new("beta", path, &error);
    g_assert_no_error(error);
    g_assert_nonnull(mailbox);

    /*
     * Read back rather than only opened.  An open that quarantined the
     * file would also "succeed", and the difference is the whole point:
     * the item queued by the older build has to still be there.
     */
    item = clawt_mailbox_get(mailbox, "old-1");
    g_assert_nonnull(item);
    g_assert_cmpstr(clawt_mailbox_item_get_body(item), ==, "from before");
    g_assert_true(clawt_mailbox_item_get_invites_reply(item));

    /* And the migrated file takes new items with the flag intact. */
    {
        g_autoptr(ClawtMailboxItem) fresh =
            clawt_mailbox_item_new("alpha", "beta", "and this one");
        g_autofree gchar *id = NULL;
        g_autoptr(ClawtMailboxItem) read_back = NULL;

        clawt_mailbox_item_set_invites_reply(fresh, FALSE);
        id = clawt_mailbox_post(mailbox, fresh, &error);
        g_assert_no_error(error);

        read_back = clawt_mailbox_get(mailbox, id);
        g_assert_nonnull(read_back);
        g_assert_false(clawt_mailbox_item_get_invites_reply(read_back));
    }

    clawt_test_remove_tree(dir);
}

/* ── The router ──────────────────────────────────────────────────── */

typedef struct {
    gchar             *dir;
    ClawtConfig       *config;
    ClawtAgentManager *agents;
    ClawtRoomManager  *rooms;
    ClawtMailboxRouter *router;
    GSocket           *far_end;
} Fixture;

static void
fixture_setup(Fixture *fixture, const gchar *agents_yaml)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *yaml = NULL;

    fixture->dir = g_dir_make_tmp("clawt-peer-XXXXXX", NULL);

    /*
     * state_dir, socket and workspace_root all inside the temporary
     * directory: the last defaults to ~/.clawtilla/agents, so a fixture
     * that leaves it out scaffolds into the developer's real fleet.
     */
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

/*
 * Gives @agent a link the drain will actually write into.
 *
 * A socketpair rather than the link server, because what is under test
 * is what clawt_mailbox_router_drain() does per item -- and a drain with
 * no open link returns without doing any of it, so a test that skipped
 * this would pass against a build where the wiring had never been added.
 *
 * The read side is never started: nothing here sends the agent's half of
 * the protocol, and an armed async reader would outlive the fixture.
 */
static void
give_agent_a_link(Fixture *fixture, ClawtAgent *agent)
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
read_delivered(Fixture *fixture)
{
    gchar buffer[8192];
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
 * A peer's reply closes the turn it starts, and says so.
 *
 * Both halves in one test on purpose.  The flag on the agent is what the
 * daemon reads when the turn ends, and the sentence in the body is the
 * only reason the agent knows not to write an answer nobody will carry
 * -- either one alone is a half-built rule, and the failure mode of the
 * missing sentence is an agent that thinks it replied.
 */
static void
test_a_peers_reply_closes_the_turn(void)
{
    Fixture fixture = { 0 };
    ClawtAgent *beta;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *delivered = NULL;

    fixture_setup(&fixture, "agents:\n  - id: alpha\n  - id: beta\n");

    beta = clawt_agent_manager_get(fixture.agents, "beta");
    g_assert_nonnull(beta);
    give_agent_a_link(&fixture, beta);

    /* alpha's turn ended, and this is what it wrote. */
    {
        g_autoptr(ClawtMessage) reply =
            clawt_message_new("beta", "alpha",
                              "Acknowledged, nothing further.");

        clawt_message_set_depth(reply, 2);
        clawt_message_set_invites_reply(reply, FALSE);

        g_assert_cmpint(clawt_mailbox_router_send(fixture.router, reply,
                                                  &error), >, 0);
        g_assert_no_error(error);
    }

    /*
     * The router drains on send, so by here beta has been handed it.
     * FALSE is what the daemon checks before routing whatever beta
     * writes at the end of this turn.
     */
    g_assert_false(clawt_agent_get_turn_replies(beta));

    delivered = read_delivered(&fixture);
    g_assert_nonnull(delivered);

    /* It says the exchange is closed, and names the way out of it. */
    g_assert_nonnull(strstr(delivered, "closes the exchange"));
    g_assert_nonnull(strstr(delivered, "clawtilla_message_agent"));

    /*
     * And not the other text.  Without this the test passes against a
     * build that sends the inviting preamble to everybody, which is the
     * behaviour being replaced.
     */
    g_assert_null(strstr(delivered, "ends the exchange -- they will read"));

    fixture_teardown(&fixture);
}

/*
 * A message somebody chose to send earns an answer, and is told it is
 * the last one.
 */
static void
test_a_deliberate_message_earns_one_answer(void)
{
    Fixture fixture = { 0 };
    ClawtAgent *beta;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *delivered = NULL;

    fixture_setup(&fixture, "agents:\n  - id: alpha\n  - id: beta\n");

    beta = clawt_agent_manager_get(fixture.agents, "beta");
    give_agent_a_link(&fixture, beta);

    g_assert_cmpint(
        clawt_mailbox_router_send_to(fixture.router, "alpha", "beta",
                                     "Can you check the build?", NULL, 1,
                                     &error), >, 0);
    g_assert_no_error(error);

    g_assert_true(clawt_agent_get_turn_replies(beta));

    delivered = read_delivered(&fixture);
    g_assert_nonnull(delivered);
    g_assert_nonnull(strstr(delivered, "another agent in your fleet"));
    g_assert_nonnull(strstr(delivered, "ends the exchange"));

    /*
     * And it no longer tells the agent to reach its operator with
     * clawtilla_message_user, which that tool refuses during a turn a
     * peer started: advice a caller will be refused for taking sends
     * the reader to a layer that is not the problem.
     */
    g_assert_null(strstr(delivered, "clawtilla_message_user"));

    fixture_teardown(&fixture);
}

/*
 * A question and an acknowledgement behind it get a turn each.
 *
 * This asserted that the *drain* left the agent free to answer, on the
 * premise that a drain hands over everything waiting and the agent
 * answers all of it in one turn -- so a bare "thanks, got it" queued
 * behind a question would close the turn the question opened and the
 * question would go unanswered.
 *
 * libreclaw does not work that way. LcSession keeps its own GQueue and
 * drain_next_message() pops one entry per turn, so two messages are two
 * turns. The premise was never true, and the accumulation it justified
 * was itself the bug: the acknowledgement's own turn inherited the
 * question's answer and replied to an acknowledgement, which is the
 * politeness loop the flag exists to end.
 *
 * So the question is still answered -- that half was always the point --
 * and now the acknowledgement is not.
 */
static void
test_a_question_survives_an_acknowledgement_behind_it(void)
{
    Fixture fixture = { 0 };
    ClawtAgent *beta;
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture, "agents:\n  - id: alpha\n  - id: beta\n");

    beta = clawt_agent_manager_get(fixture.agents, "beta");

    /*
     * Queued before beta has a link, so both are waiting when the drain
     * finally runs -- which is the situation this is about.
     */
    g_assert_cmpint(
        clawt_mailbox_router_send_to(fixture.router, "alpha", "beta",
                                     "which image did you use?", NULL, 1,
                                     &error), >, 0);
    g_assert_no_error(error);

    {
        g_autoptr(ClawtMessage) note =
            clawt_message_new("beta", "alpha", "(never mind the last one)");

        clawt_message_set_depth(note, 2);
        clawt_message_set_invites_reply(note, FALSE);

        g_assert_cmpint(clawt_mailbox_router_send(fixture.router, note,
                                                  &error), >, 0);
        g_assert_no_error(error);
    }

    give_agent_a_link(&fixture, beta);
    clawt_mailbox_router_drain(fixture.router, "beta");

    /* The question's turn answers it. */
    clawt_agent_begin_turn(beta);
    g_assert_true(clawt_agent_get_turn_replies(beta));
    g_assert_cmpstr(clawt_agent_get_turn_origin(beta), ==, "alpha");

    /* The acknowledgement's turn has nowhere to send anything. */
    clawt_agent_begin_turn(beta);
    g_assert_false(clawt_agent_get_turn_replies(beta));
    g_assert_cmpstr(clawt_agent_get_turn_origin(beta), ==, "alpha");

    /* And nothing is left behind: a third turn is a fresh chain. */
    clawt_agent_begin_turn(beta);
    g_assert_true(clawt_agent_get_turn_replies(beta));
    g_assert_null(clawt_agent_get_turn_origin(beta));

    fixture_teardown(&fixture);
}

/*
 * Every message in a burst describes its own turn.
 *
 * A peer that sends several messages while the agent is busy has them
 * all delivered at once -- delivery acknowledges at the socket, so the
 * mailbox empties long before the turns run -- and libreclaw then runs a
 * turn each.  clawtilla armed the whole burst with one boolean saying "a
 * delivery set the next turn up", which the first turn spent, so turns
 * two onwards looked like turns nothing had delivered into: depth back
 * to zero, so max_hops could not be reached; free to reply, so a closed
 * exchange was answered; and no origin, so clawtilla_message_user's
 * guard did not fire and a peer's business landed in the operator's
 * chat.  One question produced three messages there.
 *
 * Four, at four different depths, because the failure is invisible at
 * one and ambiguous at two.
 */
static void
test_every_message_in_a_burst_gets_its_own_turn(void)
{
    Fixture fixture = { 0 };
    ClawtAgent *beta;
    g_autoptr(GError) error = NULL;
    gint depth;

    fixture_setup(&fixture, "agents:\n  - id: alpha\n  - id: beta\n");

    beta = clawt_agent_manager_get(fixture.agents, "beta");

    for (depth = 1; depth <= 4; depth++) {
        g_assert_cmpint(
            clawt_mailbox_router_send_to(fixture.router, "alpha", "beta",
                                         "another one", NULL, depth,
                                         &error), >, 0);
        g_assert_no_error(error);
    }

    give_agent_a_link(&fixture, beta);
    clawt_mailbox_router_drain(fixture.router, "beta");

    for (depth = 1; depth <= 4; depth++) {
        clawt_agent_begin_turn(beta);

        g_assert_cmpint(clawt_agent_get_hop_depth(beta), ==, depth);
        g_assert_cmpstr(clawt_agent_get_turn_origin(beta), ==, "alpha");
        g_assert_true(clawt_agent_get_turn_replies(beta));
    }

    /* Four deliveries, four turns, and the fifth is nobody's. */
    clawt_agent_begin_turn(beta);
    g_assert_cmpint(clawt_agent_get_hop_depth(beta), ==, 0);
    g_assert_null(clawt_agent_get_turn_origin(beta));

    fixture_teardown(&fixture);
}

/*
 * A burst too big to remember folds rather than forgets.
 *
 * The queue is fed by whatever a peer sends and drained by turns that
 * each cost a model call, so it has to be bounded.  Dropping the oldest
 * entry outright would lose whatever it said, and the field that matters
 * is the one that closes an exchange: an agent that never learns it has
 * been answered answers back.  So the oldest is merged into the one
 * behind it, keeping the more restrictive answer.
 */
static void
test_an_overlong_burst_folds_the_oldest_in(void)
{
    Fixture fixture = { 0 };
    ClawtAgent *beta;
    GLogLevelFlags fatal;
    guint i;

    fixture_setup(&fixture, "agents:\n  - id: beta\n");

    beta = clawt_agent_manager_get(fixture.agents, "beta");

    /* A closed exchange first, then far more than fit behind it. */
    clawt_agent_deliver_turn(beta, 3, FALSE, "alpha", NULL);

    /*
     * Each fold warns, which is the point of it -- an agent this far
     * behind is worth saying so about.  Swallowed rather than avoided,
     * and restored immediately: left at 0 it would make every later test
     * in this binary ignore a real warning.
     */
    fatal = g_log_set_always_fatal(G_LOG_LEVEL_ERROR);

    for (i = 0; i < 200; i++)
        clawt_agent_deliver_turn(beta, 1, TRUE, "alpha", NULL);

    g_log_set_always_fatal(fatal);

    /*
     * The close survived the fold: the first turn out is still one whose
     * closing text goes nowhere, and it still carries the deeper hop
     * count.  Dropping instead of merging would answer it.
     */
    clawt_agent_begin_turn(beta);
    g_assert_false(clawt_agent_get_turn_replies(beta));
    g_assert_cmpint(clawt_agent_get_hop_depth(beta), ==, 3);

    fixture_teardown(&fixture);
}

/*
 * A person always gets an answer.
 *
 * The rule is about agents talking among themselves.  An operator's
 * message is a request whose whole point is the reply, and the flag has
 * to be ignored on that path however it was set -- a fleet that went
 * quiet on its human would be a far worse bug than the one being fixed.
 */
static void
test_an_operator_always_gets_an_answer(void)
{
    Fixture fixture = { 0 };
    ClawtAgent *beta;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *delivered = NULL;

    fixture_setup(&fixture, "agents:\n  - id: beta\n");

    beta = clawt_agent_manager_get(fixture.agents, "beta");
    give_agent_a_link(&fixture, beta);

    /*
     * "user" is nobody's agent id, so this is not a peer -- and the flag
     * is cleared, which on a peer would have closed the turn.
     */
    {
        g_autoptr(ClawtMessage) ask =
            clawt_message_new("beta", "user", "say hi to alpha");

        clawt_message_set_invites_reply(ask, FALSE);

        g_assert_cmpint(clawt_mailbox_router_send(fixture.router, ask,
                                                  &error), >, 0);
        g_assert_no_error(error);
    }

    g_assert_true(clawt_agent_get_turn_replies(beta));

    /* And it is handed the message itself, with no preamble at all. */
    delivered = read_delivered(&fixture);
    g_assert_nonnull(delivered);
    g_assert_null(strstr(delivered, "[clawtilla]"));
    g_assert_nonnull(strstr(delivered, "say hi to alpha"));

    fixture_teardown(&fixture);
}

/*
 * A turn nothing was delivered into answers normally.
 *
 * An agent reached over Matrix, or woken by a routine, has had no
 * delivery to set the flag -- so it has to be reset rather than left
 * holding whatever the last peer exchange put there, or one closed
 * exchange would silence the agent for every unrelated turn afterwards.
 *
 * Two turns, because one cannot tell the two states apart: the flag has
 * to survive the turn it was set for and be gone by the next.
 */
static void
test_a_turn_with_no_delivery_answers(void)
{
    Fixture fixture = { 0 };
    ClawtAgent *beta;

    fixture_setup(&fixture, "agents:\n  - id: beta\n");

    beta = clawt_agent_manager_get(fixture.agents, "beta");

    /* A delivery closes the turn it set up... */
    clawt_agent_set_turn_replies(beta, FALSE);

    clawt_agent_begin_turn(beta);
    g_assert_false(clawt_agent_get_turn_replies(beta));

    /* ...and only that one. The next turn nothing preceded answers. */
    clawt_agent_begin_turn(beta);
    g_assert_true(clawt_agent_get_turn_replies(beta));

    fixture_teardown(&fixture);
}


/*
 * Each turn knows which task it is answering, and only that one.
 *
 * This is what makes clawtilla_delegate able to record a parent: the
 * tool asks the agent what its running turn is for.  A burst of
 * [task delivery, ordinary message] must not hang the ordinary
 * message's turn off the task, or work the agent delegates from an
 * unrelated turn is parented onto somebody's job -- and the daemon
 * completes a task from the message that ends its turn, so the wrong
 * task would then be marked done by a message that was never about it.
 */
static void
test_each_turn_carries_its_own_task(void)
{
    Fixture fixture = { 0 };
    ClawtAgent *beta;
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture, "agents:\n  - id: alpha\n  - id: beta\n");

    beta = clawt_agent_manager_get(fixture.agents, "beta");

    g_assert_cmpint(clawt_mailbox_router_send_to(fixture.router, "alpha",
                                                 "beta", "do the survey",
                                                 "task-one", 1, &error),
                    >, 0);
    g_assert_no_error(error);

    g_assert_cmpint(clawt_mailbox_router_send_to(fixture.router, "alpha",
                                                 "beta", "and by the way",
                                                 NULL, 1, &error), >, 0);
    g_assert_no_error(error);

    g_assert_cmpint(clawt_mailbox_router_send_to(fixture.router, "alpha",
                                                 "beta", "this one too",
                                                 "task-two", 1, &error),
                    >, 0);
    g_assert_no_error(error);

    give_agent_a_link(&fixture, beta);
    clawt_mailbox_router_drain(fixture.router, "beta");

    clawt_agent_begin_turn(beta);
    g_assert_cmpstr(clawt_agent_get_turn_task_id(beta), ==, "task-one");

    /*
     * The middle one is nobody's task.  Carried over, it would parent
     * anything this turn delegates onto task-one -- and it is the field
     * a scalar gets wrong first, because the last delivery wins.
     */
    clawt_agent_begin_turn(beta);
    g_assert_null(clawt_agent_get_turn_task_id(beta));

    clawt_agent_begin_turn(beta);
    g_assert_cmpstr(clawt_agent_get_turn_task_id(beta), ==, "task-two");

    /* And a turn nothing delivered into is not working on anything. */
    clawt_agent_begin_turn(beta);
    g_assert_null(clawt_agent_get_turn_task_id(beta));

    fixture_teardown(&fixture);
}

/*
 * A fold does not hand one delivery's task to another.
 *
 * Everything else in a folded entry is merged towards the more
 * restrictive answer, because those fields describe a disposition.  A
 * task id names one particular piece of work, so inheriting it would
 * attribute a later message to somebody else's task -- and a task that
 * ends early is a lie where one that ends late is only a delay.
 */
static void
test_a_fold_does_not_inherit_a_task(void)
{
    Fixture fixture = { 0 };
    ClawtAgent *beta;
    GLogLevelFlags fatal;
    guint i;

    fixture_setup(&fixture, "agents:\n  - id: beta\n");

    beta = clawt_agent_manager_get(fixture.agents, "beta");

    clawt_agent_deliver_turn(beta, 3, FALSE, "alpha", "task-one");

    fatal = g_log_set_always_fatal(G_LOG_LEVEL_ERROR);

    for (i = 0; i < 200; i++)
        clawt_agent_deliver_turn(beta, 1, TRUE, "alpha", NULL);

    g_log_set_always_fatal(fatal);

    /*
     * The close and the depth survived the fold, as they must; the task
     * did not, which is equally deliberate.
     */
    clawt_agent_begin_turn(beta);
    g_assert_false(clawt_agent_get_turn_replies(beta));
    g_assert_cmpint(clawt_agent_get_hop_depth(beta), ==, 3);
    g_assert_null(clawt_agent_get_turn_task_id(beta));

    fixture_teardown(&fixture);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/peer-reply/burst-gets-a-turn-each",
                    test_every_message_in_a_burst_gets_its_own_turn);
    g_test_add_func("/peer-reply/overlong-burst-folds",
                    test_an_overlong_burst_folds_the_oldest_in);
    g_test_add_func("/peer-reply/message-default",
                    test_a_message_invites_a_reply_by_default);
    g_test_add_func("/peer-reply/item-default",
                    test_an_item_invites_a_reply_by_default);
    g_test_add_func("/peer-reply/survives-a-restart",
                    test_the_flag_survives_a_restart);
    g_test_add_func("/peer-reply/older-mailbox-migrates",
                    test_a_mailbox_from_an_older_build_migrates);
    g_test_add_func("/peer-reply/a-reply-closes-the-turn",
                    test_a_peers_reply_closes_the_turn);
    g_test_add_func("/peer-reply/one-answer-per-message",
                    test_a_deliberate_message_earns_one_answer);
    g_test_add_func("/peer-reply/a-question-outranks-an-acknowledgement",
                    test_a_question_survives_an_acknowledgement_behind_it);
    g_test_add_func("/peer-reply/an-operator-always-gets-an-answer",
                    test_an_operator_always_gets_an_answer);
    g_test_add_func("/peer-reply/a-fresh-turn-answers",
                    test_a_turn_with_no_delivery_answers);

    g_test_add_func("/peer-reply/each-turn-carries-its-own-task",
                    test_each_turn_carries_its_own_task);
    g_test_add_func("/peer-reply/a-fold-does-not-inherit-a-task",
                    test_a_fold_does_not_inherit_a_task);

    return g_test_run();
}
