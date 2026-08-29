/*
 * test-handoff.c - Giving a task away, and being able to say what happened
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A handoff has two halves that fail differently, so the tests are in
 * two halves too.
 *
 * The **store** is where a promise is kept: an agent told "this will run
 * when your turn ends" has stopped owning the work at that instant, so
 * the queue has to survive a restart, and a receipt has to outlive
 * #ClawtTaskManager -- which is in memory and forgets everything.  Those
 * tests reopen the same file, because "it survives a restart" asserted
 * against a live object is not an assertion about anything.
 *
 * The **daemon** half is about timing.  A handoff must not run while the
 * turn that asked for it is still going, must run when it ends, must
 * wait for a busy recipient a bounded number of times and then *report*
 * rather than vanish, and must be dropped if the turn was interrupted --
 * a turn somebody stopped did not finish deciding.  Every one of those
 * is a test that reaches the condition rather than asserting the code
 * exists.
 *
 * The daemon half includes core/clawt-daemon-private.h directly, exactly
 * as tests/test-turn-hygiene.c does and for the same reason: that header
 * *is* the interface of src/core/daemon-handoff.c, and settling a turn
 * is something libreclaw's typing frame does, which a hermetic test has
 * no libreclaw to produce.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>
#include <string.h>

#include "clawt-test-util.h"

#include "core/clawt-daemon-private.h"

/* ── The store ───────────────────────────────────────────────────── */

typedef struct {
    gchar             *dir;
    gchar             *path;
    ClawtHandoffStore *store;
} StoreFixture;

static void
store_setup(StoreFixture *fixture)
{
    g_autoptr(GError) error = NULL;

    fixture->dir = g_dir_make_tmp("clawt-handoff-XXXXXX", NULL);
    fixture->path = g_build_filename(fixture->dir, "handoffs.db", NULL);
    fixture->store = clawt_handoff_store_new(fixture->path, &error);

    g_assert_no_error(error);
    g_assert_nonnull(fixture->store);
}

static void
store_teardown(StoreFixture *fixture)
{
    g_clear_object(&fixture->store);

    if (fixture->dir != NULL)
        clawt_test_remove_tree(fixture->dir);

    g_clear_pointer(&fixture->dir, g_free);
    g_clear_pointer(&fixture->path, g_free);
}

static ClawtHandoff *
queue_one(StoreFixture *fixture, const gchar *task_id, const gchar *from,
          const gchar *to, gint depth)
{
    ClawtHandoff *handoff = clawt_handoff_new(task_id, from, to,
                                              "because it is yours");
    g_autoptr(GError) error = NULL;

    clawt_handoff_set_depth(handoff, depth);

    g_assert_true(clawt_handoff_store_queue(fixture->store, handoff, &error));
    g_assert_no_error(error);

    return handoff;
}

/* Every field written comes back, including the ones with no getter use
 * outside the daemon -- a column bound to the wrong index is silent. */
static void
test_a_queued_handoff_round_trips(void)
{
    StoreFixture fixture = { 0 };
    g_autoptr(ClawtHandoff) queued = NULL;
    g_autoptr(ClawtHandoff) read = NULL;

    store_setup(&fixture);

    queued = queue_one(&fixture, "task-1", "chief", "worker", 3);
    read = clawt_handoff_store_get(fixture.store,
                                   clawt_handoff_get_id(queued));

    g_assert_nonnull(read);
    g_assert_cmpstr(clawt_handoff_get_task_id(read), ==, "task-1");
    g_assert_cmpstr(clawt_handoff_get_from_agent(read), ==, "chief");
    g_assert_cmpstr(clawt_handoff_get_to_agent(read), ==, "worker");
    g_assert_cmpstr(clawt_handoff_get_reason(read), ==,
                    "because it is yours");
    g_assert_cmpint(clawt_handoff_get_depth(read), ==, 3);
    g_assert_cmpint(clawt_handoff_get_state(read), ==, CLAWT_HANDOFF_QUEUED);
    g_assert_false(clawt_handoff_is_settled(read));

    store_teardown(&fixture);
}

/*
 * The count the fan-out cap is compared against is per agent.
 *
 * A limit that counted the whole fleet's queue would refuse one chief
 * because another was busy handing things out, which is a limit about
 * the wrong thing.
 */
static void
test_the_queue_count_is_per_agent(void)
{
    StoreFixture fixture = { 0 };
    g_autoptr(ClawtHandoff) a = NULL;
    g_autoptr(ClawtHandoff) b = NULL;
    g_autoptr(ClawtHandoff) c = NULL;

    store_setup(&fixture);

    a = queue_one(&fixture, "task-1", "chief", "worker", 1);
    b = queue_one(&fixture, "task-2", "chief", "worker", 1);
    c = queue_one(&fixture, "task-3", "lead", "worker", 1);

    g_assert_cmpuint(clawt_handoff_store_count_queued(fixture.store, "chief"),
                     ==, 2);
    g_assert_cmpuint(clawt_handoff_store_count_queued(fixture.store, "lead"),
                     ==, 1);
    g_assert_cmpuint(clawt_handoff_store_count_queued(fixture.store, NULL),
                     ==, 3);

    store_teardown(&fixture);
}

/*
 * A settled handoff leaves the queue and stays readable.
 *
 * Both halves matter: still counted as queued it would be run twice, and
 * unreadable it would be a promise with no record.
 */
static void
test_settling_leaves_the_queue_and_keeps_the_receipt(void)
{
    StoreFixture fixture = { 0 };
    g_autoptr(ClawtHandoff) handoff = NULL;
    g_autoptr(ClawtHandoff) read = NULL;
    g_autoptr(GError) error = NULL;

    store_setup(&fixture);

    handoff = queue_one(&fixture, "task-1", "chief", "worker", 1);

    clawt_handoff_set_verdict(handoff, "worker was still mid-turn");
    clawt_handoff_set_state(handoff, CLAWT_HANDOFF_BUSY_GAVE_UP);

    g_assert_true(clawt_handoff_store_update(fixture.store, handoff,
                                             &error));
    g_assert_no_error(error);

    g_assert_cmpuint(clawt_handoff_store_count_queued(fixture.store, "chief"),
                     ==, 0);

    read = clawt_handoff_store_get(fixture.store,
                                   clawt_handoff_get_id(handoff));

    g_assert_cmpint(clawt_handoff_get_state(read), ==,
                    CLAWT_HANDOFF_BUSY_GAVE_UP);
    g_assert_cmpstr(clawt_handoff_get_verdict(read), ==,
                    "worker was still mid-turn");
    g_assert_cmpint(clawt_handoff_get_settled_at(read), >, 0);

    store_teardown(&fixture);
}

/*
 * The receipt is still there after the daemon that wrote it has gone.
 *
 * This is the whole reason the store exists: tasks are held in memory,
 * so after a restart a receipt is the only answer clawtilla has to "what
 * became of the thing I handed over" -- and an agent that reads silence
 * as "it never happened" hands the same work over again.
 *
 * Asserted by closing the store and opening the same file, because a
 * durability claim checked against a live object checks nothing.
 */
static void
test_a_receipt_survives_a_restart(void)
{
    StoreFixture fixture = { 0 };
    g_autoptr(ClawtHandoff) handoff = NULL;
    g_autoptr(ClawtHandoffStore) reopened = NULL;
    g_autoptr(GPtrArray) history = NULL;
    g_autoptr(GError) error = NULL;
    ClawtHandoff *read;

    store_setup(&fixture);

    handoff = queue_one(&fixture, "task-1", "chief", "worker", 2);
    clawt_handoff_set_verdict(handoff, "task-1 now belongs to worker");
    clawt_handoff_set_state(handoff, CLAWT_HANDOFF_DONE);
    g_assert_true(clawt_handoff_store_update(fixture.store, handoff, NULL));

    g_clear_object(&fixture.store);

    reopened = clawt_handoff_store_new(fixture.path, &error);
    g_assert_no_error(error);

    history = clawt_handoff_store_for_task(reopened, "task-1");
    g_assert_cmpuint(history->len, ==, 1);

    read = g_ptr_array_index(history, 0);
    g_assert_cmpint(clawt_handoff_get_state(read), ==, CLAWT_HANDOFF_DONE);
    g_assert_cmpstr(clawt_handoff_get_verdict(read), ==,
                    "task-1 now belongs to worker");

    store_teardown(&fixture);
}

/*
 * A handoff queued before a restart is still queued after it.
 *
 * It stopped being the source's problem the moment the tool answered, so
 * losing it would be losing work an agent has already been told is on
 * its way -- and it would be told so by a tool that cannot be asked
 * again, because the turn that called it is over.
 */
static void
test_the_queue_survives_a_restart(void)
{
    StoreFixture fixture = { 0 };
    g_autoptr(ClawtHandoff) handoff = NULL;
    g_autoptr(ClawtHandoffStore) reopened = NULL;
    g_autoptr(GPtrArray) queued = NULL;
    g_autoptr(GError) error = NULL;

    store_setup(&fixture);

    handoff = queue_one(&fixture, "task-9", "chief", "worker", 1);
    g_clear_object(&fixture.store);

    reopened = clawt_handoff_store_new(fixture.path, &error);
    g_assert_no_error(error);

    queued = clawt_handoff_store_queued_from(reopened, NULL);
    g_assert_cmpuint(queued->len, ==, 1);
    g_assert_cmpstr(clawt_handoff_get_task_id(
                        g_ptr_array_index(queued, 0)), ==, "task-9");

    store_teardown(&fixture);
}

/*
 * A task's handoffs come back in the order they happened.
 *
 * That order *is* the ownership history, and a history that is not in
 * order is a history that says the wrong agent had it last.
 */
static void
test_the_history_of_one_task_is_in_order(void)
{
    StoreFixture fixture = { 0 };
    g_autoptr(ClawtHandoff) first = NULL;
    g_autoptr(ClawtHandoff) second = NULL;
    g_autoptr(GPtrArray) history = NULL;

    store_setup(&fixture);

    first = queue_one(&fixture, "task-1", "chief", "worker", 1);
    clawt_handoff_set_state(first, CLAWT_HANDOFF_DONE);
    g_assert_true(clawt_handoff_store_update(fixture.store, first, NULL));

    second = queue_one(&fixture, "task-1", "worker", "scholar", 2);

    history = clawt_handoff_store_for_task(fixture.store, "task-1");

    /*
     * Both were made in the same second, which is the ordinary case:
     * timestamps here are whole seconds. So this asserts the tiebreak as
     * much as the sort -- with the id as the tiebreak, and an id being
     * random, the two came back in whichever order the hex happened to
     * fall.
     */
    g_assert_cmpuint(history->len, ==, 2);
    g_assert_cmpstr(clawt_handoff_get_to_agent(
                        g_ptr_array_index(history, 0)), ==, "worker");
    g_assert_cmpstr(clawt_handoff_get_to_agent(
                        g_ptr_array_index(history, 1)), ==, "scholar");

    store_teardown(&fixture);
}

/*
 * Pruning drops old receipts and **never** a queued handoff.
 *
 * Age means "this finished a while ago" for a receipt and "nothing has
 * drained this yet" for a queue entry. Reading the second as the first
 * would silently delete work an agent was told was on its way, which is
 * exactly the failure the queue is durable to avoid.
 */
static void
test_pruning_spares_the_queue(void)
{
    StoreFixture fixture = { 0 };
    g_autoptr(ClawtHandoff) old_receipt = NULL;
    g_autoptr(ClawtHandoff) still_queued = NULL;
    g_autoptr(GPtrArray) queued = NULL;
    g_autoptr(GPtrArray) history = NULL;
    guint removed;

    store_setup(&fixture);

    old_receipt = queue_one(&fixture, "task-old", "chief", "worker", 1);
    clawt_handoff_set_state(old_receipt, CLAWT_HANDOFF_DONE);
    clawt_handoff_set_settled_at(old_receipt,
                                 g_get_real_time() / G_USEC_PER_SEC -
                                     (10 * 24 * 60 * 60));
    g_assert_true(clawt_handoff_store_update(fixture.store, old_receipt,
                                             NULL));

    /* Queued, and older than the window: it must survive anyway. */
    still_queued = queue_one(&fixture, "task-new", "chief", "worker", 1);
    clawt_handoff_set_created_at(still_queued, 1);

    removed = clawt_handoff_store_prune(fixture.store, 2 * 24 * 60 * 60);

    /*
     * The queue is checked *before* the count, so that a prune which ate
     * a queued row fails on the sentence this test is about rather than
     * on an arithmetic mismatch a reader has to decode.
     */
    queued = clawt_handoff_store_queued_from(fixture.store, NULL);
    g_assert_cmpuint(queued->len, ==, 1);
    g_assert_cmpstr(clawt_handoff_get_task_id(
                        g_ptr_array_index(queued, 0)), ==, "task-new");

    history = clawt_handoff_store_for_task(fixture.store, "task-old");
    g_assert_cmpuint(history->len, ==, 0);

    g_assert_cmpuint(removed, ==, 1);

    store_teardown(&fixture);
}

/* ── Owner history on the task ───────────────────────────────────── */

/*
 * A task's owners are recorded in order, with the current one last.
 *
 * Seeded with the first assignee rather than left empty, so the history
 * and clawt_task_get_assignee() cannot disagree about who has it now.
 */
static void
test_owner_history_is_recorded_in_order(void)
{
    g_autoptr(ClawtTask) task = clawt_task_new("chief", "worker",
                                               "do the thing");
    GPtrArray *owners;

    owners = clawt_task_get_owner_history(task);
    g_assert_cmpuint(owners->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(owners, 0), ==, "worker");

    g_assert_true(clawt_task_transfer_owner(task, "scholar"));
    g_assert_true(clawt_task_transfer_owner(task, "chief"));

    owners = clawt_task_get_owner_history(task);
    g_assert_cmpuint(owners->len, ==, 3);
    g_assert_cmpstr(g_ptr_array_index(owners, 0), ==, "worker");
    g_assert_cmpstr(g_ptr_array_index(owners, 1), ==, "scholar");
    g_assert_cmpstr(g_ptr_array_index(owners, 2), ==, "chief");
    g_assert_cmpstr(clawt_task_get_assignee(task), ==, "chief");
}

/*
 * Handing a task to whoever already has it is not a move.
 *
 * Recorded as one it would read as a round trip that never happened, and
 * a chief reading its own history would conclude the work had bounced.
 */
static void
test_transferring_to_the_current_owner_is_not_a_move(void)
{
    g_autoptr(ClawtTask) task = clawt_task_new("chief", "worker", "work");

    g_assert_false(clawt_task_transfer_owner(task, "worker"));
    g_assert_cmpuint(clawt_task_get_owner_history(task)->len, ==, 1);
}

/* A copy carries the history, rather than sharing or losing it. */
static void
test_a_copied_task_carries_its_history(void)
{
    g_autoptr(ClawtTask) task = clawt_task_new("chief", "worker", "work");
    g_autoptr(ClawtTask) copy = NULL;

    clawt_task_transfer_owner(task, "scholar");
    copy = clawt_task_copy(task);

    g_assert_cmpuint(clawt_task_get_owner_history(copy)->len, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(clawt_task_get_owner_history(copy), 1),
                    ==, "scholar");

    /* And the two are independent: a copy is not a view. */
    clawt_task_transfer_owner(task, "chief");
    g_assert_cmpuint(clawt_task_get_owner_history(copy)->len, ==, 2);
}

/* ── The daemon ──────────────────────────────────────────────────── */

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

    fixture->dir = g_dir_make_tmp("clawt-handoffd-XXXXXX", NULL);
    fixture->config_path = g_build_filename(fixture->dir, "config.yaml",
                                            NULL);

    /*
     * Five things pinned, each of which otherwise escapes into the
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

static void
mark_busy(Fixture *fixture, const gchar *agent_id, gboolean busy)
{
    ClawtAgent *agent = clawt_agent_manager_get(
        clawt_daemon_get_agents(fixture->daemon), agent_id);

    g_assert_nonnull(agent);
    clawt_agent_set_activity(agent, busy, NULL);
}

/* Calls one orchestration tool as an agent, and returns what it said. */
static gchar *
call_tool(Fixture *fixture, const gchar *agent_id, const gchar *tool_name,
          const gchar *arguments_json, gboolean *out_is_error)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autofree gchar *request = NULL;
    g_autoptr(JsonNode) response = NULL;
    JsonObject *result;
    JsonArray *content;

    request = g_strdup_printf(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"%s\",\"arguments\":%s}}",
        tool_name, arguments_json);

    g_assert_true(json_parser_load_from_data(parser, request, -1, NULL));

    response = clawt_mcp_tools_call(
        clawt_daemon_get_mcp_tools(fixture->daemon), agent_id,
        json_parser_get_root(parser));

    g_assert_nonnull(response);

    result = json_object_get_object_member(json_node_get_object(response),
                                           "result");
    content = json_object_get_array_member(result, "content");

    if (out_is_error != NULL)
        *out_is_error = json_object_get_boolean_member(result, "isError");

    return g_strdup(json_object_get_string_member(
        json_array_get_object_element(content, 0), "text"));
}

static ClawtTask *
make_task(Fixture *fixture, const gchar *origin, const gchar *assignee)
{
    g_autoptr(GError) error = NULL;
    ClawtTask *task = clawt_task_manager_create(
        clawt_daemon_get_tasks(fixture->daemon), origin, assignee,
        "write the summary", NULL, &error);

    g_assert_no_error(error);
    g_assert_nonnull(task);

    return task;
}

static gchar *
handoff_json(const gchar *task_id, const gchar *to)
{
    return g_strdup_printf(
        "{\"task_id\":\"%s\",\"agent_id\":\"%s\","
        "\"reason\":\"you know this area\"}", task_id, to);
}

/* The fleet used by most of the daemon tests below. */
#define TWO_AGENTS \
    "agents:\n" \
    "  - id: chief\n" \
    "    chief_of_staff: true\n" \
    "  - id: worker\n"

/*
 * A handoff does not run while the turn that asked for it is going, and
 * runs when that turn ends.
 *
 * The first half is the one that is easy to get wrong and hard to see: a
 * transfer that ran immediately would move the task out from under an
 * agent that is still writing about it.
 */
static void
test_a_handoff_runs_when_the_source_turn_settles(void)
{
    Fixture fixture = { 0 };
    ClawtTask *task;
    g_autofree gchar *task_id = NULL;
    g_autofree gchar *arguments = NULL;
    g_autofree gchar *answer = NULL;
    gboolean is_error = TRUE;

    fixture_setup(&fixture, TWO_AGENTS);

    task = make_task(&fixture, "chief", "chief");
    task_id = g_strdup(clawt_task_get_id(task));
    arguments = handoff_json(task_id, "worker");

    mark_busy(&fixture, "chief", TRUE);

    answer = call_tool(&fixture, "chief", "clawtilla_handoff", arguments,
                       &is_error);

    g_assert_false(is_error);
    g_assert_nonnull(strstr(answer, "when this turn ends"));

    /* Nothing has moved yet. */
    g_assert_cmpstr(clawt_task_get_assignee(task), ==, "chief");

    mark_busy(&fixture, "chief", FALSE);
    clawt_daemon_turn_settle(fixture.daemon, "chief");

    g_assert_cmpstr(clawt_task_get_assignee(task), ==, "worker");
    g_assert_cmpuint(clawt_task_get_owner_history(task)->len, ==, 2);

    fixture_teardown(&fixture);
}

/*
 * The exchange is visible in the pair's room and in both agents' own
 * threads.
 *
 * Agent-to-agent turns cost real money, and a hidden exchange is the
 * mistake peer coordination exists to prevent. Written through
 * clawt_mailbox_router_note(), which delivers to nobody: reporting a
 * handoff must not itself start a turn.
 */
static void
test_the_exchange_is_mirrored_where_it_can_be_seen(void)
{
    Fixture fixture = { 0 };
    ClawtRoomManager *rooms;
    ClawtTask *task;
    ClawtRoom *pair;
    ClawtRoom *chief_thread;
    ClawtRoom *worker_thread;
    g_autofree gchar *task_id = NULL;
    g_autofree gchar *arguments = NULL;
    g_autofree gchar *answer = NULL;

    fixture_setup(&fixture, TWO_AGENTS);

    rooms = clawt_daemon_get_rooms(fixture.daemon);

    task = make_task(&fixture, "chief", "chief");
    task_id = g_strdup(clawt_task_get_id(task));
    arguments = handoff_json(task_id, "worker");

    answer = call_tool(&fixture, "chief", "clawtilla_handoff", arguments,
                       NULL);
    clawt_daemon_turn_settle(fixture.daemon, "chief");

    pair = clawt_room_manager_get_direct(rooms, "chief", "worker");
    chief_thread = clawt_room_manager_get_direct(rooms, "user", "chief");
    worker_thread = clawt_room_manager_get_direct(rooms, "user", "worker");

    g_assert_nonnull(pair);
    g_assert_cmpuint(clawt_room_get_message_count(pair), >, 0);
    g_assert_cmpuint(clawt_room_get_message_count(chief_thread), >, 0);
    g_assert_cmpuint(clawt_room_get_message_count(worker_thread), >, 0);

    fixture_teardown(&fixture);
}

/*
 * A busy recipient is retried a bounded number of times and then
 * *reported*.
 *
 * Never dropped: "nobody was free" and "it went wrong" call for
 * different things from whoever reads the receipt, and a handoff that
 * silently disappeared would leave a chief believing the work had moved.
 *
 * The count is checks, not seconds -- a queued handoff is looked at
 * every time any turn in the fleet ends.
 */
static void
test_a_busy_recipient_is_retried_then_reported(void)
{
    Fixture fixture = { 0 };
    ClawtTask *task;
    g_autofree gchar *task_id = NULL;
    g_autofree gchar *arguments = NULL;
    g_autofree gchar *answer = NULL;
    g_autoptr(GPtrArray) history = NULL;
    ClawtHandoff *receipt;
    guint attempt;

    fixture_setup(&fixture,
                  "orchestration:\n  handoff_busy_retries: 3\n"
                  TWO_AGENTS);

    task = make_task(&fixture, "chief", "chief");
    task_id = g_strdup(clawt_task_get_id(task));
    arguments = handoff_json(task_id, "worker");

    answer = call_tool(&fixture, "chief", "clawtilla_handoff", arguments,
                       NULL);

    mark_busy(&fixture, "worker", TRUE);

    /*
     * Three checks find the recipient busy and leave it queued. Asserted
     * per check rather than only at the end: a limit that gave up on the
     * first try would pass an end-state assertion just as well.
     */
    for (attempt = 0; attempt < 3; attempt++) {
        clawt_daemon_turn_settle(fixture.daemon, "chief");

        g_assert_cmpuint(clawt_handoff_store_count_queued(
                             fixture.daemon->handoffs, "chief"), ==, 1);
        g_assert_cmpstr(clawt_task_get_assignee(task), ==, "chief");
    }

    /* The fourth reports rather than waiting for ever. */
    clawt_daemon_turn_settle(fixture.daemon, "chief");

    g_assert_cmpuint(clawt_handoff_store_count_queued(
                         fixture.daemon->handoffs, "chief"), ==, 0);

    history = clawt_handoff_store_for_task(fixture.daemon->handoffs,
                                           task_id);
    g_assert_cmpuint(history->len, ==, 1);

    receipt = g_ptr_array_index(history, 0);
    g_assert_cmpint(clawt_handoff_get_state(receipt), ==,
                    CLAWT_HANDOFF_BUSY_GAVE_UP);
    g_assert_nonnull(strstr(clawt_handoff_get_verdict(receipt),
                            "still mid-turn"));

    /* And the work is still the giver's, which the verdict has to say. */
    g_assert_cmpstr(clawt_task_get_assignee(task), ==, "chief");
    g_assert_nonnull(strstr(clawt_handoff_get_verdict(receipt),
                            "still yours"));

    fixture_teardown(&fixture);
}

/*
 * The fan-out cap is reached, and the refusal is a sentence.
 *
 * A blocking ask gets backpressure for free because the caller waits; an
 * asynchronous handoff does not, so this number is the only thing
 * between a confused chief and a fan-out of real turns. `too_many` would
 * give the model nothing to do -- naming the count and suggesting doing
 * the piece does.
 */
static void
test_the_fifth_handoff_in_a_turn_is_refused_with_a_sentence(void)
{
    Fixture fixture = { 0 };
    guint i;

    fixture_setup(&fixture,
                  "orchestration:\n  handoff_max_per_turn: 4\n"
                  TWO_AGENTS);

    mark_busy(&fixture, "chief", TRUE);

    for (i = 0; i < 4; i++) {
        ClawtTask *task = make_task(&fixture, "chief", "chief");
        g_autofree gchar *arguments = handoff_json(clawt_task_get_id(task),
                                                   "worker");
        gboolean is_error = TRUE;
        g_autofree gchar *answer = call_tool(&fixture, "chief",
                                             "clawtilla_handoff", arguments,
                                             &is_error);

        g_assert_false(is_error);
        g_assert_nonnull(answer);
    }

    {
        ClawtTask *task = make_task(&fixture, "chief", "chief");
        g_autofree gchar *arguments = handoff_json(clawt_task_get_id(task),
                                                   "worker");
        gboolean is_error = FALSE;
        g_autofree gchar *answer = call_tool(&fixture, "chief",
                                             "clawtilla_handoff", arguments,
                                             &is_error);

        g_assert_true(is_error);
        g_assert_nonnull(strstr(answer, "4 handoffs waiting"));
        g_assert_nonnull(strstr(answer, "do this piece yourself"));
    }

    fixture_teardown(&fixture);
}

/*
 * An interrupted turn's queue is dropped, with a receipt each.
 *
 * A turn somebody stopped did not finish deciding, so carrying out the
 * transfers it had queued would be acting on half a decision. This is
 * deliberately unlike the steer queue, which survives an interrupt: a
 * steer is what somebody typed *instead*, and a handoff is part of what
 * was stopped.
 *
 * The drop is exercised through its own entry point rather than through
 * clawt_daemon_interrupt_agent(), which needs a running libreclaw child
 * this suite has no way to produce. What the test can still prove is the
 * part that would actually break: that a drop taken before a settle wins
 * over the settle, which is the ordering the interrupt path depends on.
 */
static void
test_an_interrupted_turn_drops_its_queue_with_a_receipt(void)
{
    Fixture fixture = { 0 };
    ClawtTask *first;
    ClawtTask *second;
    g_autofree gchar *first_id = NULL;
    g_autofree gchar *second_id = NULL;
    g_autoptr(GPtrArray) history = NULL;
    guint i;

    fixture_setup(&fixture, TWO_AGENTS);

    mark_busy(&fixture, "chief", TRUE);

    first = make_task(&fixture, "chief", "chief");
    second = make_task(&fixture, "chief", "chief");
    first_id = g_strdup(clawt_task_get_id(first));
    second_id = g_strdup(clawt_task_get_id(second));

    for (i = 0; i < 2; i++) {
        g_autofree gchar *arguments =
            handoff_json(i == 0 ? first_id : second_id, "worker");
        g_autofree gchar *answer = call_tool(&fixture, "chief",
                                             "clawtilla_handoff", arguments,
                                             NULL);

        g_assert_nonnull(answer);
    }

    clawt_daemon_handoff_drop_queued(fixture.daemon, "chief",
                                     "the turn was interrupted");

    /* Nothing left to run, so the settle that follows carries none of it. */
    mark_busy(&fixture, "chief", FALSE);
    clawt_daemon_turn_settle(fixture.daemon, "chief");

    g_assert_cmpstr(clawt_task_get_assignee(first), ==, "chief");
    g_assert_cmpstr(clawt_task_get_assignee(second), ==, "chief");

    /* A receipt per item, not one for the batch. */
    for (i = 0; i < 2; i++) {
        ClawtHandoff *receipt;

        g_clear_pointer(&history, g_ptr_array_unref);
        history = clawt_handoff_store_for_task(
            fixture.daemon->handoffs, i == 0 ? first_id : second_id);

        g_assert_cmpuint(history->len, ==, 1);

        receipt = g_ptr_array_index(history, 0);
        g_assert_cmpint(clawt_handoff_get_state(receipt), ==,
                        CLAWT_HANDOFF_DROPPED);
        g_assert_nonnull(strstr(clawt_handoff_get_verdict(receipt),
                                "interrupted"));
    }

    fixture_teardown(&fixture);
}

/*
 * A handoff whose recipient has been removed is dropped, not run.
 *
 * Everything is re-read at run time for exactly this: an agent can go
 * away between the call and the turn boundary, and a handoff that
 * trusted its arguments would move a task onto a list nobody owns.
 */
static void
test_a_handoff_to_a_removed_agent_is_dropped(void)
{
    Fixture fixture = { 0 };
    ClawtTask *task;
    g_autofree gchar *task_id = NULL;
    g_autofree gchar *arguments = NULL;
    g_autofree gchar *answer = NULL;
    g_autoptr(GPtrArray) history = NULL;
    ClawtHandoff *receipt;

    fixture_setup(&fixture, TWO_AGENTS);

    task = make_task(&fixture, "chief", "chief");
    task_id = g_strdup(clawt_task_get_id(task));
    arguments = handoff_json(task_id, "worker");

    mark_busy(&fixture, "chief", TRUE);
    answer = call_tool(&fixture, "chief", "clawtilla_handoff", arguments,
                       NULL);
    g_assert_nonnull(answer);

    /*
     * Removed the way `agent.remove` does it: out of the config, then
     * reloaded. There is no manager-level removal, and inventing one
     * here would be testing a path the daemon does not take.
     */
    g_assert_true(clawt_config_remove_agent(
        clawt_daemon_get_config(fixture.daemon), "worker"));
    clawt_agent_manager_load(clawt_daemon_get_agents(fixture.daemon), NULL);

    mark_busy(&fixture, "chief", FALSE);
    clawt_daemon_turn_settle(fixture.daemon, "chief");

    g_assert_cmpstr(clawt_task_get_assignee(task), ==, "chief");

    history = clawt_handoff_store_for_task(fixture.daemon->handoffs,
                                           task_id);
    g_assert_cmpuint(history->len, ==, 1);

    receipt = g_ptr_array_index(history, 0);
    g_assert_cmpint(clawt_handoff_get_state(receipt), ==,
                    CLAWT_HANDOFF_DROPPED);
    g_assert_nonnull(strstr(clawt_handoff_get_verdict(receipt),
                            "no longer an agent"));

    fixture_teardown(&fixture);
}

/*
 * Handing a task to yourself, to nobody, or when it is not yours.
 *
 * Each refusal names what to do instead. An agent told only "no" tries
 * the same thing in a different shape, which is how one bad sentence in
 * a tool description costs a fleet a day.
 */
static void
test_the_refusals_say_what_to_do_instead(void)
{
    Fixture fixture = { 0 };
    ClawtTask *mine;
    ClawtTask *theirs;
    g_autofree gchar *mine_id = NULL;
    g_autofree gchar *theirs_id = NULL;

    fixture_setup(&fixture,
        "agents:\n"
        "  - id: chief\n"
        "    chief_of_staff: true\n"
        "  - id: worker\n"
        "  - id: scholar\n");

    mine = make_task(&fixture, "chief", "chief");
    theirs = make_task(&fixture, "worker", "scholar");
    mine_id = g_strdup(clawt_task_get_id(mine));
    theirs_id = g_strdup(clawt_task_get_id(theirs));

    {
        g_autofree gchar *arguments = handoff_json(mine_id, "chief");
        gboolean is_error = FALSE;
        g_autofree gchar *answer = call_tool(&fixture, "chief",
                                             "clawtilla_handoff", arguments,
                                             &is_error);

        g_assert_true(is_error);
        g_assert_nonnull(strstr(answer, "That is you"));
    }

    {
        g_autofree gchar *arguments = handoff_json(mine_id, "nobody");
        gboolean is_error = FALSE;
        g_autofree gchar *answer = call_tool(&fixture, "chief",
                                             "clawtilla_handoff", arguments,
                                             &is_error);

        g_assert_true(is_error);
        g_assert_nonnull(strstr(answer, "clawtilla_list_agents"));
    }

    {
        g_autofree gchar *arguments = handoff_json(theirs_id, "worker");
        gboolean is_error = FALSE;
        g_autofree gchar *answer = call_tool(&fixture, "chief",
                                             "clawtilla_handoff", arguments,
                                             &is_error);

        g_assert_true(is_error);
        g_assert_nonnull(strstr(answer, "not yours to hand on"));
    }

    {
        g_autofree gchar *arguments = NULL;
        gboolean is_error = FALSE;
        g_autofree gchar *answer = NULL;

        clawt_task_manager_complete(clawt_daemon_get_tasks(fixture.daemon),
                                    mine_id, "done");
        arguments = handoff_json(mine_id, "worker");
        answer = call_tool(&fixture, "chief", "clawtilla_handoff", arguments,
                           &is_error);

        g_assert_true(is_error);
        g_assert_nonnull(strstr(answer, "already ended"));
    }

    fixture_teardown(&fixture);
}

/*
 * The reason is required, and the refusal says why it is not paperwork.
 *
 * It is the only context the recipient gets: they cannot see the
 * conversation the handoff was decided in.
 */
static void
test_a_handoff_without_a_reason_is_refused(void)
{
    Fixture fixture = { 0 };
    ClawtTask *task;
    g_autofree gchar *arguments = NULL;
    g_autofree gchar *answer = NULL;
    gboolean is_error = FALSE;

    fixture_setup(&fixture, TWO_AGENTS);

    task = make_task(&fixture, "chief", "chief");
    arguments = g_strdup_printf(
        "{\"task_id\":\"%s\",\"agent_id\":\"worker\"}",
        clawt_task_get_id(task));

    answer = call_tool(&fixture, "chief", "clawtilla_handoff", arguments,
                       &is_error);

    g_assert_true(is_error);
    g_assert_nonnull(strstr(answer, "reason"));
    g_assert_nonnull(strstr(answer, "context"));

    fixture_teardown(&fixture);
}

/*
 * The delivery is stamped one hop beyond the turn that asked.
 *
 * Read at run time the depth would be whatever the giver's *next* turn
 * had, or zero -- and a handoff delivered at zero restarts the chain, so
 * a task could be passed round the fleet for ever without
 * orchestration.max_hops ever being reached.
 */
static void
test_the_depth_is_taken_from_the_turn_that_asked(void)
{
    Fixture fixture = { 0 };
    ClawtAgent *chief;
    ClawtTask *task;
    g_autofree gchar *arguments = NULL;
    g_autofree gchar *answer = NULL;
    g_autoptr(GPtrArray) queued = NULL;

    fixture_setup(&fixture, TWO_AGENTS);

    chief = clawt_agent_manager_get(clawt_daemon_get_agents(fixture.daemon),
                                    "chief");
    clawt_agent_set_hop_depth(chief, 2);
    mark_busy(&fixture, "chief", TRUE);

    task = make_task(&fixture, "chief", "chief");
    arguments = handoff_json(clawt_task_get_id(task), "worker");
    answer = call_tool(&fixture, "chief", "clawtilla_handoff", arguments,
                       NULL);
    g_assert_nonnull(answer);

    queued = clawt_handoff_store_queued_from(fixture.daemon->handoffs,
                                             "chief");
    g_assert_cmpuint(queued->len, ==, 1);
    g_assert_cmpint(clawt_handoff_get_depth(g_ptr_array_index(queued, 0)),
                    ==, 3);

    fixture_teardown(&fixture);
}

/*
 * clawtilla_task_status answers from the receipts when the task itself
 * has gone.
 *
 * "There is no task X" to an agent that handed one over reads as "your
 * handoff never happened", and that is how you get two of everything.
 */
static void
test_task_status_answers_from_a_receipt(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtHandoff) receipt = NULL;
    g_autofree gchar *answer = NULL;
    gboolean is_error = TRUE;

    fixture_setup(&fixture, TWO_AGENTS);

    receipt = clawt_handoff_new("task-gone", "chief", "worker", "yours now");
    clawt_handoff_set_verdict(receipt,
                              "task-gone now belongs to worker");
    g_assert_true(clawt_handoff_store_queue(fixture.daemon->handoffs,
                                            receipt, NULL));
    clawt_handoff_set_state(receipt, CLAWT_HANDOFF_DONE);
    g_assert_true(clawt_handoff_store_update(fixture.daemon->handoffs,
                                             receipt, NULL));

    answer = call_tool(&fixture, "chief", "clawtilla_task_status",
                       "{\"task_id\":\"task-gone\"}", &is_error);

    g_assert_false(is_error);
    g_assert_nonnull(strstr(answer, "no longer in memory"));
    g_assert_nonnull(strstr(answer, "chief -> worker"));
    g_assert_nonnull(strstr(answer, "now belongs to worker"));

    fixture_teardown(&fixture);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/handoff/store/round-trip",
                    test_a_queued_handoff_round_trips);
    g_test_add_func("/handoff/store/count-is-per-agent",
                    test_the_queue_count_is_per_agent);
    g_test_add_func("/handoff/store/settling-keeps-the-receipt",
                    test_settling_leaves_the_queue_and_keeps_the_receipt);
    g_test_add_func("/handoff/store/receipt-survives-a-restart",
                    test_a_receipt_survives_a_restart);
    g_test_add_func("/handoff/store/queue-survives-a-restart",
                    test_the_queue_survives_a_restart);
    g_test_add_func("/handoff/store/history-in-order",
                    test_the_history_of_one_task_is_in_order);
    g_test_add_func("/handoff/store/pruning-spares-the-queue",
                    test_pruning_spares_the_queue);

    g_test_add_func("/handoff/task/owner-history",
                    test_owner_history_is_recorded_in_order);
    g_test_add_func("/handoff/task/no-self-move",
                    test_transferring_to_the_current_owner_is_not_a_move);
    g_test_add_func("/handoff/task/copy-carries-history",
                    test_a_copied_task_carries_its_history);

    g_test_add_func("/handoff/runs-at-settle",
                    test_a_handoff_runs_when_the_source_turn_settles);
    g_test_add_func("/handoff/mirrored-into-the-rooms",
                    test_the_exchange_is_mirrored_where_it_can_be_seen);
    g_test_add_func("/handoff/busy-retried-then-reported",
                    test_a_busy_recipient_is_retried_then_reported);
    g_test_add_func("/handoff/fan-out-cap",
                    test_the_fifth_handoff_in_a_turn_is_refused_with_a_sentence);
    g_test_add_func("/handoff/interrupt-drops-the-queue",
                    test_an_interrupted_turn_drops_its_queue_with_a_receipt);
    g_test_add_func("/handoff/removed-agent-is-dropped",
                    test_a_handoff_to_a_removed_agent_is_dropped);
    g_test_add_func("/handoff/refusals-say-what-to-do",
                    test_the_refusals_say_what_to_do_instead);
    g_test_add_func("/handoff/reason-is-required",
                    test_a_handoff_without_a_reason_is_refused);
    g_test_add_func("/handoff/depth-from-the-asking-turn",
                    test_the_depth_is_taken_from_the_turn_that_asked);
    g_test_add_func("/handoff/status-answers-from-a-receipt",
                    test_task_status_answers_from_a_receipt);

    return g_test_run();
}
