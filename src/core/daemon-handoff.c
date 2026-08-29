/*
 * daemon-handoff.c - Moving ownership of a task, after the turn that asked
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * `clawtilla_handoff` queues; this runs the queue.  Three properties are
 * the whole of the design, and each of them is a bug somewhere else in
 * this tree that was paid for once already:
 *
 *   **It runs at a turn boundary.**  An agent asking for a handoff must
 *   not wait on the recipient, because the recipient may be mid-turn and
 *   a turn is minutes.  So the tool answers immediately and
 *   clawt_daemon_turn_settle() drains the queue -- which also means the
 *   source has finished writing whatever it was writing before the task
 *   moves out from under it.
 *
 *   **Everything is re-read before it runs.**  Between queueing and
 *   running, an agent can be deleted, a team can be re-arranged, the
 *   task can be completed by somebody else and the recipient can start
 *   another turn.  A handoff that trusted the arguments it was given
 *   would start a turn on an agent that is now busy, or resurrect a task
 *   that has ended.  This is the same rule the decision inbox already
 *   has for anything a person could sleep through.
 *
 *   **Every outcome leaves a receipt and says something in the room.**
 *   Agent-to-agent turns cost real money, and an exchange nobody can see
 *   is the mistake peer coordination exists to prevent.  The receipts
 *   are durable because #ClawtTaskManager is not: after a restart they
 *   are the only answer clawtilla has to "what happened to the thing I
 *   handed over", and an agent that reads silence as "it never happened"
 *   hands the same work over again.
 */

#include "clawtilla.h"

#include "core/clawt-daemon.h"
#include "core/clawt-daemon-private.h"

/*
 * Where the queue and the receipts live.
 *
 * Beside the mailboxes and the decision inbox, under the state directory
 * the daemon already holds a flock on -- one daemon owns this at a time,
 * and that is what the lock is for.
 */
#define HANDOFF_DATABASE_NAME "handoffs.db"

static gint64
receipt_keep_seconds(ClawtDaemon *self)
{
    gint64 days;

    if (self->config == NULL)
        return 0;

    days = clawt_config_get_int(self->config,
                                "orchestration.handoff_receipt_days");

    return (days > 0) ? days * 24 * 60 * 60 : 0;
}

static guint
busy_retries(ClawtDaemon *self)
{
    gint64 retries;

    if (self->config == NULL)
        return 0;

    retries = clawt_config_get_int(self->config,
                                   "orchestration.handoff_busy_retries");

    return (retries > 0) ? (guint)retries : 0;
}

/* ── Saying what happened ────────────────────────────────────────── */

/*
 * The pair's room, if the two have one.
 *
 * clawt_room_manager_get_direct() makes it on demand, which is right:
 * two agents that have never spoken and are now handing work to each
 * other are exactly the pair whose conversation should start existing.
 */
static const gchar *
pair_room(ClawtDaemon *self, const gchar *a, const gchar *b)
{
    ClawtRoom *room;

    if (self->rooms == NULL || a == NULL || b == NULL)
        return NULL;

    room = clawt_room_manager_get_direct(self->rooms, a, b);

    return (room != NULL) ? clawt_room_get_id(room) : NULL;
}

/*
 * Writes one line into the pair's room and into each agent's own thread.
 *
 * Through clawt_mailbox_router_note(), which appends and delivers to
 * nobody: telling somebody that a handoff failed must not itself start
 * a turn, or a fleet with a misconfigured team spends money reporting
 * that it cannot hand anything over.
 *
 * Three places rather than one because they answer different questions.
 * The pair's room is where the exchange lives and is where a person
 * reads it back; each agent's own thread is where its operator is
 * looking.  A handoff that showed up in none of them would be an
 * agent-to-agent turn nobody could see, which is the thing peer
 * coordination is supposed to make visible rather than hide.
 */
static void
announce(ClawtDaemon *self, ClawtHandoff *handoff, const gchar *text)
{
    const gchar *from = clawt_handoff_get_from_agent(handoff);
    const gchar *to = clawt_handoff_get_to_agent(handoff);
    const gchar *room;

    if (self->router == NULL)
        return;

    room = pair_room(self, from, to);

    if (room != NULL) {
        clawt_handoff_set_room(handoff, room);
        clawt_mailbox_router_note(self->router, room, text, NULL);
    }

    /*
     * The agents themselves are addressed by id, which
     * clawt_mailbox_router_note() resolves to each one's conversation
     * with its operator.  A missing agent is not an error here: the
     * commonest reason a handoff settled at all is that one of them went
     * away, and failing to announce that would lose the only record of
     * it.
     */
    if (from != NULL)
        clawt_mailbox_router_note(self->router, from, text, NULL);

    if (to != NULL)
        clawt_mailbox_router_note(self->router, to, text, NULL);
}

static void
publish_handoff(ClawtDaemon *self, const gchar *kind, ClawtHandoff *handoff)
{
    g_autoptr(ClawtEvent) event = NULL;

    if (self->bus == NULL)
        return;

    event = clawt_event_new(kind, clawt_handoff_get_task_id(handoff));
    clawt_event_set_detail(event, "handoff", clawt_handoff_get_id(handoff));
    clawt_event_set_detail(event, "from",
                           clawt_handoff_get_from_agent(handoff));
    clawt_event_set_detail(event, "to", clawt_handoff_get_to_agent(handoff));
    clawt_event_set_detail(event, "state",
                           clawt_enum_to_nick(CLAWT_TYPE_HANDOFF_STATE,
                               (gint)clawt_handoff_get_state(handoff)));

    if (clawt_handoff_get_verdict(handoff) != NULL)
        clawt_event_set_detail(event, "verdict",
                               clawt_handoff_get_verdict(handoff));

    clawt_event_bus_publish(self->bus, event);
}

/*
 * Ends one handoff, writes its receipt, and says so where it can be seen.
 *
 * One function for every terminal state, so there is no outcome that
 * reaches the store without reaching the room, or the room without
 * reaching the store.  The two drifted apart in the first draft of this
 * file, and a `denied` handoff was invisible to everybody but a reader
 * of the database.
 */
static void
settle(ClawtDaemon      *self,
       ClawtHandoff     *handoff,
       ClawtHandoffState state,
       const gchar      *verdict)
{
    g_autoptr(GError) error = NULL;

    clawt_handoff_set_verdict(handoff, verdict);
    clawt_handoff_set_state(handoff, state);

    announce(self, handoff, verdict);

    if (self->handoffs != NULL &&
        !clawt_handoff_store_update(self->handoffs, handoff, &error))
        g_warning("handoff: could not record the outcome of %s: %s",
                  clawt_handoff_get_id(handoff),
                  error != NULL ? error->message : "unknown");

    publish_handoff(self, "handoff.settled", handoff);
}

/* ── Running one ─────────────────────────────────────────────────── */

/*
 * Carries out a handoff that is at the front of the queue.
 *
 * Returns %TRUE when the handoff has settled one way or another, %FALSE
 * when it is to stay queued and be tried again.
 */
static gboolean
run_one(ClawtDaemon *self, ClawtHandoff *handoff)
{
    const gchar *from = clawt_handoff_get_from_agent(handoff);
    const gchar *to = clawt_handoff_get_to_agent(handoff);
    const gchar *task_id = clawt_handoff_get_task_id(handoff);
    ClawtAgent *giver;
    ClawtAgent *taker;
    ClawtTask *task;
    g_autofree gchar *body = NULL;
    g_autoptr(GError) error = NULL;

    /*
     * Everything is looked up again here rather than trusted from the
     * queue row.  Between the call and this moment a whole turn has
     * gone by, and on the restart path days may have.
     */
    task = (self->tasks != NULL)
        ? clawt_task_manager_get(self->tasks, task_id) : NULL;

    if (task == NULL) {
        g_autofree gchar *verdict = g_strdup_printf(
            "Handing task %s to %s was dropped: the task is no longer being "
            "tracked. Tasks are held in memory, so a restart between the "
            "handoff and this point loses them.", task_id, to);

        settle(self, handoff, CLAWT_HANDOFF_DROPPED, verdict);
        return TRUE;
    }

    if (clawt_task_is_finished(task)) {
        g_autofree gchar *verdict = g_strdup_printf(
            "Handing task %s to %s was dropped: it had already ended as %s "
            "by the time the handoff ran, so there was no ownership left to "
            "move.", task_id, to,
            clawt_enum_to_nick(CLAWT_TYPE_TASK_STATE,
                               clawt_task_get_state(task)));

        settle(self, handoff, CLAWT_HANDOFF_DROPPED, verdict);
        return TRUE;
    }

    taker = (self->agents != NULL)
        ? clawt_agent_manager_get(self->agents, to) : NULL;

    if (taker == NULL) {
        g_autofree gchar *verdict = g_strdup_printf(
            "Handing task %s to %s was dropped: there is no longer an agent "
            "called %s. The task is still yours.", task_id, to, to);

        settle(self, handoff, CLAWT_HANDOFF_DROPPED, verdict);
        return TRUE;
    }

    giver = (self->agents != NULL && from != NULL)
        ? clawt_agent_manager_get(self->agents, from) : NULL;

    if (giver == NULL) {
        g_autofree gchar *verdict = g_strdup_printf(
            "Handing task %s to %s was dropped: the agent that asked for it "
            "is no longer in the fleet, so there is nobody whose permission "
            "to check.", task_id, to);

        settle(self, handoff, CLAWT_HANDOFF_DROPPED, verdict);
        return TRUE;
    }

    /*
     * The team rule again, and not because the first check was
     * unreliable: a team can be re-arranged while a handoff sits in the
     * queue, and on the restart path an operator has had the whole
     * downtime to edit clawtilla.yaml.  Enforcing it only where it was
     * convenient would be a rule about that call site.
     */
    {
        g_autofree gchar *refusal = NULL;

        if (!clawt_team_may_assign(clawt_agent_get_config(giver),
                                   clawt_agent_get_config(taker),
                                   &refusal)) {
            g_autofree gchar *verdict = g_strdup_printf(
                "Handing task %s to %s was refused: %s", task_id, to,
                refusal != NULL ? refusal
                                : "that is not yours to assign");

            settle(self, handoff, CLAWT_HANDOFF_DENIED, verdict);
            return TRUE;
        }
    }

    /*
     * A busy recipient waits rather than being interrupted.  Delivering
     * into a running turn is what the mailbox is for, but a *handoff*
     * needs the recipient to read the reason and pick the work up, and
     * an item queued behind an agent that is mid-turn arrives with
     * whatever else is queued -- so it is held here where it can still
     * be reported if the agent never frees up.
     */
    if (clawt_agent_get_busy(taker)) {
        guint attempts = clawt_handoff_get_attempts(handoff) + 1;
        guint limit = busy_retries(self);

        clawt_handoff_set_attempts(handoff, attempts);

        if (limit > 0 && attempts > limit) {
            g_autofree gchar *verdict = g_strdup_printf(
                "Handing task %s to %s was given up on: %s was still "
                "mid-turn after %u tries. Nothing went wrong and nobody "
                "refused -- they were busy. The task is still yours, so "
                "either do it or hand it to somebody else.",
                task_id, to, to, limit);

            settle(self, handoff, CLAWT_HANDOFF_BUSY_GAVE_UP, verdict);
            return TRUE;
        }

        /*
         * The attempt count is written back even though nothing has
         * settled.  A counter kept only in memory would restart at zero
         * on every daemon restart, and a handoff to an agent that is
         * permanently wedged would be retried for ever.
         */
        if (self->handoffs != NULL)
            clawt_handoff_store_update(self->handoffs, handoff, NULL);

        return FALSE;
    }

    body = g_strdup_printf(
        "[clawtilla] %s has handed you task %s. It is yours now -- they are "
        "no longer working on it.\n\nWhy you: %s\n\nWhat the task is:\n%s\n\n"
        "Report it finished with clawtilla_task_complete when it is done.",
        from, task_id,
        clawt_handoff_get_reason(handoff) != NULL
            ? clawt_handoff_get_reason(handoff) : "not said",
        clawt_task_get_prompt(task) != NULL
            ? clawt_task_get_prompt(task) : "(nothing recorded)");

    if (self->router == NULL ||
        clawt_mailbox_router_send_to(self->router, from, to, body, task_id,
                                     clawt_handoff_get_depth(handoff),
                                     &error) < 0) {
        g_autofree gchar *verdict = g_strdup_printf(
            "Handing task %s to %s failed: %s", task_id, to,
            error != NULL ? error->message
                          : "there is nowhere to route it");

        settle(self, handoff, CLAWT_HANDOFF_FAILED, verdict);
        return TRUE;
    }

    /*
     * Ownership moves only once the delivery has been accepted.
     *
     * The other order looks equivalent and is not: a transfer written
     * before a refused send leaves a task whose assignee has never been
     * told about it, which reads to everybody -- the giver, the taker
     * and a person looking at the list -- as work in progress that
     * nobody is doing.  Enqueuing is what makes the recipient certain to
     * hear about it, since the mailbox is durable and survives them
     * being stopped.
     */
    clawt_task_transfer_owner(task, to);

    {
        g_autofree gchar *verdict = g_strdup_printf(
            "Task %s now belongs to %s, handed over by %s: %s",
            task_id, to, from != NULL ? from : "somebody",
            clawt_handoff_get_reason(handoff) != NULL
                ? clawt_handoff_get_reason(handoff) : "no reason given");

        settle(self, handoff, CLAWT_HANDOFF_DONE, verdict);
    }

    return TRUE;
}

/* ── The queue ───────────────────────────────────────────────────── */

void
clawt_daemon_handoff_pump(ClawtDaemon *self)
{
    g_autoptr(GPtrArray) queued = NULL;
    guint i;
    guint settled = 0;

    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (self->handoffs == NULL)
        return;

    /*
     * Re-entrancy is real here and it is quiet.  Running a handoff
     * writes into rooms and delivers a message, both of which publish
     * events, and an event handler that ends a turn would call straight
     * back into this function -- where it would find rows it had already
     * begun and run them twice.
     */
    if (self->handoff_pumping)
        return;

    self->handoff_pumping = TRUE;

    queued = clawt_handoff_store_queued_from(self->handoffs, NULL);

    for (i = 0; queued != NULL && i < queued->len; i++) {
        ClawtHandoff *handoff = g_ptr_array_index(queued, i);
        const gchar *from = clawt_handoff_get_from_agent(handoff);
        ClawtAgent *giver = (self->agents != NULL && from != NULL)
            ? clawt_agent_manager_get(self->agents, from) : NULL;

        /*
         * A handoff runs when the turn that asked for it ends, and this
         * is where that is enforced -- the pump is called on every
         * settle in the fleet, so a source that is still mid-turn is
         * skipped rather than raced.  Without it a handoff queued by one
         * agent would run the moment any other agent finished a turn,
         * and the source would find its task gone mid-sentence.
         */
        if (giver != NULL && clawt_agent_get_busy(giver))
            continue;

        if (run_one(self, handoff))
            settled++;
    }

    /*
     * Pruned only when something settled, so a fleet full of queued
     * handoffs and an idle recipient does not run two DELETE statements
     * on every turn boundary for nothing.
     */
    if (settled > 0)
        clawt_handoff_store_prune(self->handoffs, receipt_keep_seconds(self));

    self->handoff_pumping = FALSE;
}

void
clawt_daemon_handoff_drop_queued(ClawtDaemon *self,
                                 const gchar *agent_id,
                                 const gchar *why)
{
    g_autoptr(GPtrArray) queued = NULL;
    guint i;

    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (self->handoffs == NULL || agent_id == NULL)
        return;

    queued = clawt_handoff_store_queued_from(self->handoffs, agent_id);

    for (i = 0; queued != NULL && i < queued->len; i++) {
        ClawtHandoff *handoff = g_ptr_array_index(queued, i);
        g_autofree gchar *verdict = NULL;

        verdict = g_strdup_printf(
            "Handing task %s to %s was dropped: %s. The task is still %s's.",
            clawt_handoff_get_task_id(handoff),
            clawt_handoff_get_to_agent(handoff),
            why != NULL ? why : "the turn that asked for it was stopped",
            agent_id);

        settle(self, handoff, CLAWT_HANDOFF_DROPPED, verdict);
    }
}

/* ── The hook the tool calls ─────────────────────────────────────── */

/*
 * Queues one handoff on behalf of the agent that asked for it.
 *
 * The fan-out cap lives here because only the daemon can see the queue.
 * It is small on purpose: a blocking ask gets backpressure for free
 * because the caller waits, and an asynchronous handoff does not -- so
 * this number is the only thing standing between a confused chief of
 * staff and a fan-out of real turns that each cost money.
 */
static gboolean
queue_handoff_for_tools(const gchar  *from_agent,
                        const gchar  *task_id,
                        const gchar  *to_agent,
                        const gchar  *reason,
                        guint        *out_queued,
                        gpointer      user_data,
                        GError      **error)
{
    ClawtDaemon *self = user_data;
    g_autoptr(ClawtHandoff) handoff = NULL;
    ClawtAgent *giver;
    guint already;
    gint64 limit;

    if (self->handoffs == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                            "this daemon has nowhere to record a handoff, so "
                            "it cannot promise to carry one out");
        return FALSE;
    }

    already = clawt_handoff_store_count_queued(self->handoffs, from_agent);
    limit = (self->config != NULL)
        ? clawt_config_get_int(self->config,
                               "orchestration.handoff_max_per_turn")
        : 0;

    if (limit > 0 && already >= (guint)limit) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT,
                    "you already have %u handoffs waiting to run when this "
                    "turn ends, and the limit is %" G_GINT64_FORMAT ". Each "
                    "one starts a real turn on somebody, so finish deciding "
                    "before you queue more -- or do this piece yourself.",
                    already, limit);
        return FALSE;
    }

    handoff = clawt_handoff_new(task_id, from_agent, to_agent, reason);

    /*
     * The depth is taken now, while the turn that asked is still the
     * turn running.  Read at run time it would be whatever the agent's
     * next turn had, or zero -- and a handoff delivered at zero restarts
     * the chain, so a task could be passed round the fleet for ever
     * without orchestration.max_hops ever being reached.
     */
    giver = (self->agents != NULL && from_agent != NULL)
        ? clawt_agent_manager_get(self->agents, from_agent) : NULL;

    clawt_handoff_set_depth(handoff,
                            (giver != NULL)
                                ? clawt_agent_get_hop_depth(giver) + 1 : 1);

    if (!clawt_handoff_store_queue(self->handoffs, handoff, error))
        return FALSE;

    publish_handoff(self, "handoff.queued", handoff);

    if (out_queued != NULL)
        *out_queued = already + 1;

    return TRUE;
}

/* ── Lifecycle ───────────────────────────────────────────────────── */

void
clawt_daemon_handoff_setup(ClawtDaemon *self)
{
    g_autofree gchar *path = NULL;
    g_autoptr(GError) error = NULL;

    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (self->state_dir == NULL)
        return;

    path = g_build_filename(self->state_dir, HANDOFF_DATABASE_NAME, NULL);
    self->handoffs = clawt_handoff_store_new(path, &error);

    if (self->handoffs == NULL) {
        /*
         * A warning rather than a failure to start.  A fleet with no
         * handoff store is a fleet where one tool is not offered, which
         * is a smaller problem than a daemon that will not come up --
         * and clawt_mcp_tools_is_permitted() already withholds
         * clawtilla_handoff when the hook is unset, so nothing is
         * offered and then broken.
         */
        g_warning("handoff: could not open %s, so handing tasks over is "
                  "not available: %s", path,
                  error != NULL ? error->message : "unknown");
        return;
    }

    /*
     * Old receipts go at start, because that is the one moment nobody is
     * waiting on the daemon and the file has just been opened.
     */
    clawt_handoff_store_prune(self->handoffs, receipt_keep_seconds(self));

    if (self->mcp_tools == NULL)
        return;

    clawt_mcp_tools_set_handoff_func(self->mcp_tools, queue_handoff_for_tools,
                                     self, NULL);
    clawt_mcp_tools_set_handoff_store(self->mcp_tools, self->handoffs);

    /*
     * Anything queued when the daemon last stopped runs now.  The point
     * of persisting the queue is that a handoff an agent was told was on
     * its way still happens -- it stopped being the source's problem the
     * instant the tool answered.
     */
    clawt_daemon_handoff_pump(self);
}

void
clawt_daemon_handoff_teardown(ClawtDaemon *self)
{
    g_return_if_fail(CLAWT_IS_DAEMON(self));

    /*
     * The tools lose their pointer before the store goes, or a call
     * arriving during shutdown reads a freed one.  The queue itself is
     * deliberately left on disk: a handoff that has not run has not been
     * cancelled, and the next daemon picks it up.
     */
    if (self->mcp_tools != NULL) {
        clawt_mcp_tools_set_handoff_func(self->mcp_tools, NULL, NULL, NULL);
        clawt_mcp_tools_set_handoff_store(self->mcp_tools, NULL);
    }

    g_clear_object(&self->handoffs);
}
