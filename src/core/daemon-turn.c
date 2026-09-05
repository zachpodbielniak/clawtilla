/*
 * daemon-turn.c - Ending a turn, and an exchange, that has stopped moving
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * #ClawtLoopGuard had four limits and all four **refused a message**.
 * Refusing is not ending: a stuck pair kept taking turns, each turn
 * produced a message, each message was refused -- and the turn had
 * already been paid for before the refusal arrived.  The limits were
 * being billed for the runaway they existed to prevent.
 *
 * What this file adds is the part that ends things, and it is careful
 * about which things it can honestly claim to end:
 *
 *   **Agent-to-agent ping-pong.**  clawtilla owns the mailbox router, so
 *   this really stops.  The room is stalled, the task moves to stalled,
 *   the pair is told, and one alert names both of them.
 *
 *   **A wedged turn.**  clawtilla owns the runtime, so this really stops
 *   too: the turn is interrupted, the reason goes into the thread, and a
 *   grace timer force-releases the agent if the runtime never reports the
 *   end.  That last part is the lesson `agent.restart` already learned --
 *   a stop that only signals is not a stop, and without it a second turn
 *   overlaps the one being stopped.
 *
 *   **A tool loop inside one turn.**  clawtilla owns *none* of this.  The
 *   loop that decides to call a tool again lives in the model's own CLI
 *   and there is no way to steer it from here.  So this half observes: it
 *   counts, it writes a note into the thread, and at the last threshold
 *   it escalates to the interrupt.  Saying so plainly is not modesty --
 *   a check that implies more reach than it has sends the next reader to
 *   the wrong layer.
 */

#include "clawtilla.h"

#include <string.h>

#include "core/clawt-daemon.h"
#include "core/clawt-daemon-private.h"

/*
 * How often the budgets are checked.
 *
 * Coarse on purpose.  The shortest budget anything here allows is a
 * minute, so five seconds of slop is a twelfth of the smallest thing
 * being measured, and a timer that fires every second on an idle fleet
 * is a daemon that never lets the machine sleep.
 */
#define TURN_SWEEP_INTERVAL_SECONDS (5)

/*
 * How long the daemon waits for a killed turn to report that it ended.
 *
 * An interrupt kills the CLI process running the turn; libreclaw then
 * lowers its typing indicator, which is what settles everything here.
 * When that never arrives -- and it is precisely the interrupt case where
 * it may not, because the code that lowers it was taken out from under
 * the turn -- the agent stays marked busy for ever, the next delivery
 * overlaps a turn nobody is running, and the watch never begins again.
 */
#define TURN_GRACE_SECONDS (15)

typedef struct {
    ClawtDaemon *daemon;   /* not owned: it outlives every grace timer */
    gchar       *agent_id;
} GraceJob;

static void
grace_job_free(gpointer data)
{
    GraceJob *job = data;

    g_free(job->agent_id);
    g_free(job);
}

static void
publish_stall(ClawtDaemon      *self,
              const gchar      *subject,
              const gchar      *kind,
              ClawtStallReason  reason,
              const gchar      *detail)
{
    g_autoptr(ClawtEvent) event = NULL;

    if (self->bus == NULL)
        return;

    event = clawt_event_new(kind, subject);
    clawt_event_set_detail(event, "reason",
                           clawt_enum_to_nick(CLAWT_TYPE_STALL_REASON,
                                              (gint)reason));

    if (detail != NULL)
        clawt_event_set_detail(event, "detail", detail);

    clawt_event_bus_publish(self->bus, event);
}

/* ── The grace timer ─────────────────────────────────────────────── */

static gboolean
on_grace_elapsed(gpointer data)
{
    GraceJob *job = data;
    ClawtDaemon *self = job->daemon;
    ClawtAgent *agent;

    if (self->turn_grace != NULL)
        g_hash_table_remove(self->turn_grace, job->agent_id);

    agent = (self->agents != NULL)
        ? clawt_agent_manager_get(self->agents, job->agent_id) : NULL;

    if (agent != NULL && clawt_agent_get_busy(agent)) {
        /*
         * The runtime never said the turn ended.  Saying so here rather
         * than waiting is the difference between an agent somebody can
         * talk to and one that shows a spinner until the daemon is
         * restarted -- and the next delivery would otherwise begin a
         * second turn on top of the one being stopped.
         */
        g_warning("turn: %s did not report the end of its turn %u seconds "
                  "after being interrupted; releasing it here",
                  job->agent_id, self->turn_grace_seconds);

        clawt_agent_set_activity(agent, FALSE, NULL);
        clawt_daemon_turn_settle(self, job->agent_id);
    }

    return G_SOURCE_REMOVE;
}

/*
 * The context is named here, in the function that attaches the source,
 * rather than at any of the three places that end a turn.  Naming it at
 * the call site has failed five times in this tree, and an embedded
 * daemon's timer on the wrong context simply never fires.  The gtk-doc
 * block is in clawt-daemon-private.h.
 */
void
clawt_daemon_turn_arm_grace(ClawtDaemon *self, const gchar *agent_id)
{
    GraceJob *job;
    GSource *source;

    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (self->turn_grace == NULL || agent_id == NULL)
        return;

    /* Already waiting on this agent; a second timer would fire twice. */
    if (g_hash_table_lookup(self->turn_grace, agent_id) != NULL)
        return;

    job = g_new0(GraceJob, 1);
    job->daemon = self;
    job->agent_id = g_strdup(agent_id);

    source = g_timeout_source_new_seconds(self->turn_grace_seconds);
    g_source_set_callback(source, on_grace_elapsed, job, grace_job_free);
    g_source_attach(source, self->main_context);

    g_hash_table_insert(self->turn_grace, g_strdup(agent_id), source);
}

static void
cancel_grace(ClawtDaemon *self, const gchar *agent_id)
{
    if (self->turn_grace == NULL || agent_id == NULL)
        return;

    g_hash_table_remove(self->turn_grace, agent_id);
}

static void
grace_source_free(gpointer data)
{
    GSource *source = data;

    g_source_destroy(source);
    g_source_unref(source);
}

/* ── Expiry ──────────────────────────────────────────────────────── */

/*
 * Stops one turn that has run out of budget, in the order the failures
 * demand.
 *
 * Counters first: they are keyed by the turn, and an interrupt that
 * settles the agent before they are dropped would leave the next turn
 * inheriting this one's counts and reporting a loop that is not there.
 */
static void
stop_expired_turn(ClawtDaemon      *self,
                  const gchar      *agent_id,
                  ClawtStallReason  reason,
                  const gchar      *message)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GPtrArray) tasks = NULL;
    guint killed = 0;

    if (self->repeats != NULL)
        clawt_repeat_watch_end_turn(self->repeats, agent_id);

    publish_stall(self, agent_id, "turn.timed_out", reason, message);

    if (!clawt_daemon_interrupt_agent(self, agent_id, &killed, &error)) {
        /*
         * Not a failure worth stopping for.  An agent that was already
         * stopped is an agent whose turn is already over, which is the
         * outcome this was after.
         */
        g_debug("turn: could not interrupt %s: %s", agent_id,
                error != NULL ? error->message : "unknown");
    }

    if (self->router != NULL)
        clawt_mailbox_router_note(self->router, agent_id, message, NULL);

    /*
     * And anything that was waiting on this turn.  A task left running
     * behind a turn that no longer exists is the state a chief of staff
     * reads as "still working" and waits on for ever.
     */
    if (self->tasks != NULL) {
        guint i;

        tasks = clawt_task_manager_list_involving(self->tasks, agent_id, FALSE);

        for (i = 0; tasks != NULL && i < tasks->len; i++) {
            ClawtTask *task = g_ptr_array_index(tasks, i);

            if (clawt_task_is_finished(task))
                continue;

            clawt_task_manager_stall(self->tasks,
                                     clawt_task_get_id(task), message);
        }
    }

    clawt_daemon_turn_arm_grace(self, agent_id);
}

/*
 * One agent's turn budget.
 *
 * Both the deadline and the message that reports it read this, because
 * they had drifted: the deadline came from the agent's own config and
 * the message from the watch's fleet-wide field, so a stopped agent was
 * handed a figure belonging to whichever agent began a turn last and
 * told it was `agents.runtime.turn_timeout_seconds`.
 */
static guint
agent_turn_budget(ClawtDaemon *self, const gchar *agent_id)
{
    ClawtAgentConfig *config;

    if (self->config == NULL || agent_id == NULL)
        return 0;

    config = clawt_config_get_agent(self->config, agent_id);

    if (config == NULL)
        return (guint)MAX(clawt_config_get_int(
                              self->config,
                              "agents.runtime.turn_timeout_seconds"), 0);

    return (guint)MAX(clawt_agent_config_get_int(
                          config, "runtime.turn_timeout_seconds"), 0);
}

/*
 * One member's hold on one room.
 *
 * The holder table was keyed on the room alone, which is exactly right
 * while a room has two members and one of them is the operator: there
 * is only ever one agent in it, so room and holder are the same fact.
 * A group room breaks that -- mention two agents and both take a turn
 * in the same room -- and a scalar per room then described whichever
 * had started last.  The other member was left unwatched, and either
 * one's falling edge ended the watch for both.
 *
 * The separator is a byte an id cannot contain: clawt_is_valid_id()
 * allows letters, digits, '-' and '_', and a room id additionally has
 * the daemon's own ':' prefixes, none of which is a control character.
 */
#define ROOM_HOLD_SEPARATOR "\x1f"

static gchar *
room_hold_key(const gchar *room_id, const gchar *agent_id)
{
    return g_strconcat(room_id, ROOM_HOLD_SEPARATOR, agent_id, NULL);
}

/*
 * The room half of such a key.
 *
 * Returns: (transfer full): the room id
 */
static gchar *
room_hold_room(const gchar *key)
{
    const gchar *sep = strstr(key, ROOM_HOLD_SEPARATOR);

    return (sep != NULL) ? g_strndup(key, (gsize)(sep - key))
                         : g_strdup(key);
}

/*
 * Ends one hold, by its key.
 *
 * Both settle paths go through here rather than repeating the pair of
 * calls, because they had already diverged once: the agent-wide settle
 * knew about the watch and the holder, and would have known about
 * nothing a later change added to the per-room one.
 */
static void
settle_hold(ClawtDaemon *self, const gchar *key)
{
    clawt_turn_watch_end(self->room_watch, key);

    if (self->room_holder != NULL)
        g_hash_table_remove(self->room_holder, key);
}

static gboolean on_turn_sweep(gpointer data);

/*
 * One sweep, now.
 *
 * For tests: a budget measured in minutes cannot be reached by waiting,
 * and the thing worth testing is what the daemon's handler does with an
 * overrun -- whose turn it stops and whether it ends the room -- not
 * that a timer eventually fires.
 */
void
clawt_daemon_turn_sweep_now(ClawtDaemon *self)
{
    g_return_if_fail(CLAWT_IS_DAEMON(self));

    on_turn_sweep(self);
}

static gboolean
on_turn_sweep(gpointer data)
{
    ClawtDaemon *self = data;
    g_autoptr(GPtrArray) wedged = NULL;
    g_autoptr(GPtrArray) overrun = NULL;
    guint i;

    if (self->turn_watch == NULL || self->room_watch == NULL)
        return G_SOURCE_CONTINUE;

    wedged = clawt_turn_watch_collect_expired(self->turn_watch);

    for (i = 0; i < wedged->len; i++) {
        const gchar *agent_id = g_ptr_array_index(wedged, i);
        g_autofree gchar *message = NULL;

        message = g_strdup_printf(
            "[clawtilla] This turn produced nothing for %u seconds and was "
            "stopped. That is agents.runtime.turn_timeout_seconds, which "
            "counts activity rather than elapsed time -- a turn may run for "
            "an hour while it is doing things. Whatever was in flight is "
            "gone; say what you have rather than starting again.",
            agent_turn_budget(self, agent_id));

        stop_expired_turn(self, agent_id, CLAWT_STALL_TURN_TIMEOUT, message);
    }

    overrun = clawt_turn_watch_collect_expired(self->room_watch);

    for (i = 0; i < overrun->len; i++) {
        const gchar *key = g_ptr_array_index(overrun, i);
        const gchar *holder = (self->room_holder != NULL)
            ? g_hash_table_lookup(self->room_holder, key) : NULL;
        g_autofree gchar *held = g_strdup(holder);
        g_autofree gchar *room_id = room_hold_room(key);
        g_autofree gchar *message = NULL;

        if (self->room_holder != NULL)
            g_hash_table_remove(self->room_holder, key);

        if (held == NULL)
            continue;

        message = g_strdup_printf(
            "[clawtilla] '%s' held its turn in this room for the whole "
            "budget and it has been yielded. That is "
            "rooms.turn_timeout_seconds, counted in work rather than in "
            "wall time -- the clock holds while a turn is waiting on a "
            "person.", held);

        /*
         * The member's turn is always stopped.  Whether the *room* is
         * stalled with it depends on how many other members there are.
         *
         * A pair is an exchange: one member that spent a whole budget
         * without finishing will spend the next one the same way, and
         * ending the exchange is the only thing that stops it.  A group
         * is not -- stalling it refuses every other member's posts and
         * stalls every task they hold, so one slow agent freezes a
         * standup of five, and the notice said it "held this room's
         * turn" when it held only its own.  The hold is keyed per
         * member now; the stall has to be too, and for a group the
         * honest answer is to stop that member and leave the room
         * running.
         */
        {
            ClawtRoom *room = (self->rooms != NULL)
                ? clawt_room_manager_get(self->rooms, room_id) : NULL;
            gboolean is_a_group = room != NULL && clawt_room_is_group(room);

            if (self->guard != NULL && !is_a_group)
                clawt_loop_guard_stall_room(self->guard, room_id,
                                            CLAWT_STALL_ROOM_TIMEOUT,
                                            message);

            if (is_a_group)
                clawt_mailbox_router_note(self->router, room_id, message,
                                          NULL);
        }

        stop_expired_turn(self, held, CLAWT_STALL_ROOM_TIMEOUT, message);
    }

    return G_SOURCE_CONTINUE;
}

/* ── Repeated tool calls ─────────────────────────────────────────── */

/*
 * An agent has made the same call a suspicious number of times.
 *
 * The note goes into the thread rather than into a log, because the
 * agent is the one that can do something about it and the operator is
 * the one who needs to know it happened.  At the last threshold the turn
 * is interrupted, which is the only lever clawtilla has here at all.
 */
static void
on_repeat_threshold(ClawtRepeatWatch *watch,
                    const gchar      *agent_id,
                    const gchar      *key,
                    guint             count,
                    gpointer          user_data)
{
    ClawtDaemon *self = user_data;
    g_autofree gchar *message = NULL;
    gboolean last = count >= clawt_repeat_watch_get_highest_threshold(watch);

    message = g_strdup_printf(
        "[clawtilla] '%s' has made the same call %u times in this turn: %s\n"
        "%s",
        agent_id, count, key,
        last
            ? "That is the last threshold, so the turn has been stopped."
            : "Nothing has been stopped; clawtilla cannot steer a turn that "
              "is already running. If the call is not producing a different "
              "answer, try something else.");

    publish_stall(self, agent_id, "turn.repeating",
                  CLAWT_STALL_REPEATED_TOOL_CALL, key);

    if (self->router != NULL)
        clawt_mailbox_router_note(self->router, agent_id, message, NULL);

    if (!last)
        return;

    stop_expired_turn(self, agent_id, CLAWT_STALL_REPEATED_TOOL_CALL,
                      message);
}

/* ── A stalled exchange ──────────────────────────────────────────── */

/*
 * The guard has ended an exchange.
 *
 * One alert, naming both agents and what was going round, because that
 * is the whole content of the report: an alert that says "a loop was
 * stopped" and not which loop sends whoever reads it to the transcripts
 * to work out the rest.
 */
static void
on_guard_stalled(ClawtLoopGuard *guard,
                 const gchar    *room_id,
                 guint           reason,
                 const gchar    *detail,
                 gpointer        user_data)
{
    ClawtDaemon *self = user_data;
    ClawtRoom *room;
    g_autoptr(GString) who = NULL;
    g_autofree gchar *message = NULL;
    g_autofree gchar *clipped = NULL;

    (void)guard;

    room = (self->rooms != NULL)
        ? clawt_room_manager_get(self->rooms, room_id) : NULL;

    who = g_string_new(NULL);

    if (room != NULL) {
        GPtrArray *members = clawt_room_get_members(room);
        guint i;

        for (i = 0; i < members->len; i++) {
            if (who->len > 0)
                g_string_append(who, " and ");

            g_string_append(who, g_ptr_array_index(members, i));
        }
    }

    /*
     * Clipped, because the repeated text is model output of any length
     * and an alert is a line somebody reads at a glance.  The transcript
     * still has all of it.
     */
    if (detail != NULL) {
        clipped = (g_utf8_strlen(detail, -1) > 120)
            ? g_strdup_printf("%.*s...", (gint)(g_utf8_offset_to_pointer(
                                  detail, 120) - detail), detail)
            : g_strdup(detail);
    }

    message = g_strdup_printf(
        "[clawtilla] The exchange in %s has been ended: %s kept sending the "
        "same thing and each refusal was costing a turn. What was going "
        "round: %s\nSay something here to restart it.",
        room_id,
        (who->len > 0) ? who->str : "the agents in it",
        (clipped != NULL) ? clipped : "(nothing recorded)");

    publish_stall(self, room_id, "exchange.stalled",
                  (ClawtStallReason)reason, clipped);

    if (self->router != NULL)
        clawt_mailbox_router_note(self->router, room_id, message, NULL);

    /*
     * And every task the members were working on, because a task behind
     * a stalled exchange is work nobody is doing and nothing will report.
     */
    if (self->tasks != NULL && room != NULL) {
        GPtrArray *members = clawt_room_get_members(room);
        guint i;

        for (i = 0; i < members->len; i++) {
            const gchar *member = g_ptr_array_index(members, i);
            g_autoptr(GPtrArray) tasks =
                clawt_task_manager_list_involving(self->tasks, member, FALSE);
            guint j;

            for (j = 0; tasks != NULL && j < tasks->len; j++) {
                ClawtTask *task = g_ptr_array_index(tasks, j);

                if (clawt_task_is_finished(task))
                    continue;

                clawt_task_manager_stall(self->tasks,
                                         clawt_task_get_id(task), message);
            }
        }
    }
}

/*
 * Whether a sender is an agent rather than a person.
 *
 * The guard needs this to end an exchange -- clawtilla ends conversations
 * between agents and never a person's -- and cannot answer it itself.
 */
static gboolean
sender_is_agent(const gchar *sender_id, gpointer user_data)
{
    ClawtDaemon *self = user_data;

    if (sender_id == NULL || self->agents == NULL)
        return FALSE;

    return clawt_agent_manager_get(self->agents, sender_id) != NULL;
}

/* ── Lifecycle ───────────────────────────────────────────────────── */

void
clawt_daemon_turn_setup(ClawtDaemon *self)
{
    GSource *source;

    g_return_if_fail(CLAWT_IS_DAEMON(self));

    self->repeats = clawt_repeat_watch_new();
    self->turn_watch = clawt_turn_watch_new_activity();
    self->room_watch = clawt_turn_watch_new_work();
    self->steers = clawt_steer_queue_new();

    self->room_holder = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, g_free);

    self->room_steps = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);

    if (self->turn_grace_seconds == 0)
        self->turn_grace_seconds = TURN_GRACE_SECONDS;
    self->turn_grace = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, grace_source_free);

    g_signal_connect(self->repeats, "threshold",
                     G_CALLBACK(on_repeat_threshold), self);

    if (self->guard != NULL) {
        g_signal_connect(self->guard, "stalled",
                         G_CALLBACK(on_guard_stalled), self);

        clawt_loop_guard_set_peer_func(self->guard, sender_is_agent, self,
                                       NULL);
    }

    clawt_daemon_turn_configure(self);

    /*
     * Attached to the daemon's own context by name.  g_timeout_add_seconds()
     * takes the global default, which for an embedded daemon is a loop
     * nobody runs -- so the sweep would never fire and every budget here
     * would be a number in a config file and nothing else.
     */
    source = g_timeout_source_new_seconds(TURN_SWEEP_INTERVAL_SECONDS);
    g_source_set_callback(source, on_turn_sweep, self, NULL);
    g_source_attach(source, self->main_context);
    self->turn_sweep = source;
}

void
clawt_daemon_turn_set_grace_seconds(ClawtDaemon *self, guint seconds)
{
    g_return_if_fail(CLAWT_IS_DAEMON(self));

    self->turn_grace_seconds = (seconds > 0) ? seconds : TURN_GRACE_SECONDS;
}

void
clawt_daemon_turn_configure(ClawtDaemon *self)
{
    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (self->repeats != NULL) {
        g_autofree gchar *thresholds = NULL;

        g_auto(GStrv) list = clawt_config_get_string_list(
            self->config, "orchestration.repeat_thresholds");

        thresholds = (list != NULL) ? g_strjoinv(",", list) : NULL;

        clawt_repeat_watch_set_thresholds(self->repeats, thresholds);
        clawt_repeat_watch_set_max_keys(
            self->repeats,
            (guint)clawt_config_get_int(self->config,
                                        "orchestration.repeat_max_keys"));
    }

    /*
     * The agent watch takes the fleet default here and each turn takes
     * the agent's own value at clawt_daemon_turn_begin(), because the
     * budget is per agent and the watch is one object.
     */
    if (self->turn_watch != NULL)
        clawt_turn_watch_set_budget(
            self->turn_watch,
            (guint)MAX(clawt_config_get_int(
                           self->config,
                           "agents.runtime.turn_timeout_seconds"), 0));
}

void
clawt_daemon_turn_teardown(ClawtDaemon *self)
{
    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (self->turn_sweep != NULL) {
        g_source_destroy(self->turn_sweep);
        g_clear_pointer(&self->turn_sweep, g_source_unref);
    }

    /*
     * The grace timers go before the objects they would touch.  A timer
     * left armed past this point fires into a daemon that has already
     * dropped its agent manager.
     */
    g_clear_pointer(&self->turn_grace, g_hash_table_unref);
    g_clear_pointer(&self->room_holder, g_hash_table_unref);
    g_clear_pointer(&self->room_steps, g_hash_table_unref);

    if (self->guard != NULL) {
        g_signal_handlers_disconnect_by_func(self->guard,
                                             G_CALLBACK(on_guard_stalled),
                                             self);
        clawt_loop_guard_set_peer_func(self->guard, NULL, NULL, NULL);
    }

    if (self->repeats != NULL)
        g_signal_handlers_disconnect_by_func(self->repeats,
                                             G_CALLBACK(on_repeat_threshold),
                                             self);

    g_clear_object(&self->repeats);
    g_clear_object(&self->turn_watch);
    g_clear_object(&self->room_watch);
    g_clear_object(&self->steers);
}

/* ── The turn itself ─────────────────────────────────────────────── */

void
clawt_daemon_turn_begin(ClawtDaemon *self, const gchar *agent_id,
                        const gchar *room_id)
{
    ClawtAgentConfig *config;

    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (agent_id == NULL || self->turn_watch == NULL)
        return;

    cancel_grace(self, agent_id);

    /*
     * The agent's own budget, not the fleet's.  set_budget() is the
     * allowance the begin() below stamps onto this agent's entry, which
     * then keeps it: the watch used to hold one number for every agent,
     * and since several are routinely mid-turn together that number was
     * whichever agent started last.
     *
     * @room_id is taken for the caller's convenience and no longer used
     * here: the room half is clawt_daemon_turn_begin_room(), because it
     * fires on a different edge.  This one fires when an idle agent
     * becomes busy; that one fires whenever a room's turn starts, which
     * for an agent already talking to somebody else is not the same
     * moment.
     */
    (void)room_id;
    config = (self->config != NULL)
        ? clawt_config_get_agent(self->config, agent_id) : NULL;

    if (config != NULL)
        clawt_turn_watch_set_budget(self->turn_watch,
                                    agent_turn_budget(self, agent_id));

    clawt_turn_watch_begin(self->turn_watch, agent_id);
}

void
clawt_daemon_turn_begin_room(ClawtDaemon *self, const gchar *agent_id,
                             const gchar *room_id)
{
    g_autofree gchar *key = NULL;
    ClawtRoom *room;
    guint budget;

    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (agent_id == NULL || room_id == NULL ||
        self->room_watch == NULL || self->rooms == NULL)
        return;

    room = clawt_room_manager_get(self->rooms, room_id);
    budget = (room != NULL) ? clawt_room_get_turn_timeout(room) : 0;

    if (budget == 0)
        return;

    key = room_hold_key(room_id, agent_id);

    clawt_turn_watch_set_budget(self->room_watch, budget);
    clawt_turn_watch_begin(self->room_watch, key);

    g_hash_table_insert(self->room_holder, g_steal_pointer(&key),
                        g_strdup(agent_id));
}

void
clawt_daemon_turn_settle_room(ClawtDaemon *self, const gchar *agent_id,
                              const gchar *room_id)
{
    g_autofree gchar *key = NULL;

    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (agent_id == NULL || room_id == NULL || self->room_watch == NULL)
        return;

    /*
     * The member as well as the room.  Without it, one member of a
     * group finishing ended the watch on the room -- and so on the
     * member still working, whose budget then could not be enforced
     * and whose overrun could not be named.
     */
    key = room_hold_key(room_id, agent_id);

    settle_hold(self, key);

    /*
     * The steps of the turn that just ended are *kept*.
     *
     * They used to be dropped here, on the reasoning that they describe
     * work in flight and there is no longer any.  That was wrong about
     * what a reader wants: the tools an agent reached for are how its
     * answer came about, and deleting them the moment the answer
     * arrives takes the working away and leaves the conclusion.  It
     * also read as a bug, because it is one -- text vanishing from a
     * conversation you are looking at.
     *
     * They stay in the room's bounded history instead, which is what
     * makes them survive a room switch and a client restart as well.
     */
}

void
clawt_daemon_turn_activity(ClawtDaemon *self, const gchar *agent_id)
{
    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (self->turn_watch != NULL)
        clawt_turn_watch_note_activity(self->turn_watch, agent_id);
}

/*
 * Everything that ends when a turn does.
 *
 * Idempotent, because three things settle a turn -- the runtime's own
 * typing frame, the interrupt verb, and the grace timer -- and any two of
 * them can arrive together.
 */
void
clawt_daemon_turn_settle(ClawtDaemon *self, const gchar *agent_id)
{
    g_autofree gchar *thread_id = NULL;
    g_autofree gchar *steered = NULL;

    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (agent_id == NULL)
        return;

    cancel_grace(self, agent_id);

    /*
     * Whatever the runtime still believes it is typing, it is not in a
     * turn now.
     *
     * Three things settle a turn and only one of them is the runtime's
     * own FALSE; an interrupt and the grace timer both end turns the
     * runtime will never close itself.  Leaving the set standing after
     * one of those would mean the agent's next real frame is not a
     * rising edge, so clawt_agent_begin_turn() would be skipped and the
     * new turn would run holding the abandoned turn's depth, origin and
     * task -- the same wrong answer as before, arrived at from the
     * other direction.
     */
    if (self->agents != NULL) {
        ClawtAgent *agent = clawt_agent_manager_get(self->agents, agent_id);

        if (agent != NULL)
            clawt_agent_clear_typing(agent);
    }

    if (self->turn_watch != NULL)
        clawt_turn_watch_end(self->turn_watch, agent_id);

    if (self->repeats != NULL)
        clawt_repeat_watch_end_turn(self->repeats, agent_id);

    /*
     * Every room this agent was holding, not the first one found.
     *
     * An agent talking to three peers holds three rooms at once, so a
     * loop that broke on the first match left the other two watched with
     * a holder who had stopped -- and the next sweep would collect them
     * and stall a room over a turn that had already ended.  The break
     * was correct only while an agent could hold one room, which is what
     * the room edge now makes possible to see.
     */
    if (self->room_holder != NULL && self->room_watch != NULL) {
        g_autoptr(GPtrArray) held = g_ptr_array_new_with_free_func(g_free);
        GHashTableIter iter;
        gpointer key;
        gpointer value;
        guint i;

        g_hash_table_iter_init(&iter, self->room_holder);

        while (g_hash_table_iter_next(&iter, &key, &value)) {
            if (g_strcmp0(value, agent_id) == 0)
                g_ptr_array_add(held, g_strdup(key));
        }

        /*
         * Collected first, then removed: g_hash_table_remove() during an
         * iteration is undefined, and g_hash_table_iter_remove() would
         * still leave the keys we are about to read freed underneath us.
         */
        /*
         * Through the per-room settle rather than repeating its body.
         * The two had already diverged once in the making: this loop
         * knew about the watch and the holder, and nothing here would
         * have known about anything a later change added to the other
         * one -- which is the shape that leaves a rule enforced at one
         * call site instead of in the function.
         */
        for (i = 0; i < held->len; i++)
            settle_hold(self, g_ptr_array_index(held, i));
    }

    /*
     * And any handoff waiting on a turn boundary.
     *
     * Every settle, not only this agent's own queue: a handoff held
     * because its recipient was mid-turn is retried when *that* agent
     * finishes, and that settle belongs to somebody else.  Draining only
     * the settling agent's own rows would leave a handoff waiting for a
     * turn its source will never take again.
     */
    clawt_daemon_handoff_pump(self);

    if (self->steers == NULL)
        return;

    /*
     * And anything typed at the agent while it was working.  Drained in
     * a loop rather than once, because two conversations can each be
     * holding something; each becomes one follow-up turn.
     */
    for (;;) {
        g_autoptr(GError) error = NULL;

        g_clear_pointer(&thread_id, g_free);
        g_clear_pointer(&steered, g_free);

        steered = clawt_steer_queue_drain(self->steers, agent_id, &thread_id);

        if (steered == NULL)
            break;

        if (self->bus != NULL) {
            g_autoptr(ClawtEvent) changed =
                clawt_event_new("mailbox.follow-ups-changed", agent_id);

            clawt_event_bus_publish(self->bus, changed);
        }

        if (self->router == NULL)
            break;

        if (clawt_mailbox_router_send_to(self->router, "user", agent_id,
                                         steered, NULL, 0, &error) < 0)
            g_warning("steer: could not deliver to %s: %s", agent_id,
                      error != NULL ? error->message : "unknown");
    }
}

void
clawt_daemon_turn_hold(ClawtDaemon *self, const gchar *agent_id)
{
    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (agent_id == NULL)
        return;

    if (self->turn_watch != NULL)
        clawt_turn_watch_hold(self->turn_watch, agent_id);

    if (self->room_holder == NULL || self->room_watch == NULL)
        return;

    {
        GHashTableIter iter;
        gpointer key;
        gpointer value;

        g_hash_table_iter_init(&iter, self->room_holder);

        /*
         * Every room the agent holds, not the first.  An agent parked on
         * one decision is parked in all of them -- it is one process --
         * so holding only one room's clock let the others run down while
         * nothing was happening.
         */
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            if (g_strcmp0(value, agent_id) == 0)
                clawt_turn_watch_hold(self->room_watch, key);
        }
    }
}

void
clawt_daemon_turn_release(ClawtDaemon *self, const gchar *agent_id)
{
    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (agent_id == NULL)
        return;

    if (self->turn_watch != NULL)
        clawt_turn_watch_release(self->turn_watch, agent_id);

    if (self->room_holder == NULL || self->room_watch == NULL)
        return;

    {
        GHashTableIter iter;
        gpointer key;
        gpointer value;

        g_hash_table_iter_init(&iter, self->room_holder);

        /*
         * Every room the agent holds, not the first.  An agent parked on
         * one decision is parked in all of them -- it is one process --
         * so holding only one room's clock let the others run down while
         * nothing was happening.
         */
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            if (g_strcmp0(value, agent_id) == 0)
                clawt_turn_watch_release(self->room_watch, key);
        }
    }
}

void
clawt_daemon_turn_note_tool_call(ClawtDaemon *self,
                                 const gchar *agent_id,
                                 const gchar *tool,
                                 const gchar *args)
{
    g_return_if_fail(CLAWT_IS_DAEMON(self));

    /*
     * A tool call is the clearest sign of life the daemon gets.  An
     * agent's own bash and read never reach here -- they belong to the
     * CLI -- so this and an outbound message are the whole of what
     * "activity" can honestly mean on this side of the socket.
     */
    clawt_daemon_turn_activity(self, agent_id);

    if (self->repeats != NULL)
        clawt_repeat_watch_note(self->repeats, agent_id, tool, args);
}

/* ── Steering ────────────────────────────────────────────────────── */

gboolean
clawt_daemon_turn_steer(ClawtDaemon *self,
                        const gchar *from,
                        const gchar *target,
                        const gchar *body)
{
    ClawtAgent *agent;
    ClawtRoom *room;

    g_return_val_if_fail(CLAWT_IS_DAEMON(self), FALSE);

    if (self->steers == NULL || target == NULL || body == NULL)
        return FALSE;

    /*
     * Only a person steers.  A message from one agent to another is
     * ordinary traffic and belongs in the mailbox, which is durable and
     * ordered precisely so that a busy recipient is not a special case.
     */
    if (from != NULL && g_strcmp0(from, "user") != 0)
        return FALSE;

    agent = (self->agents != NULL)
        ? clawt_agent_manager_get(self->agents, target) : NULL;

    /*
     * A stopped agent falls through to the mailbox.  That is what the
     * mailbox is for, and holding the message here instead would lose it
     * -- nothing settles a turn that never started.
     */
    if (agent == NULL || !clawt_agent_get_busy(agent))
        return FALSE;

    room = clawt_room_manager_get_direct(self->rooms, "user", target);

    if (room == NULL)
        return FALSE;

    /*
     * Held out of the transcript on purpose.  Appending it now would make
     * the queued line the active leaf, so the rest of the turn already in
     * flight would hang off a line the model was never shown -- and the
     * transcript would read as though the agent had answered something
     * nobody said.
     */
    clawt_steer_queue_add(self->steers, clawt_room_get_id(room), target,
                          body);

    if (self->bus != NULL) {
        g_autoptr(ClawtEvent) event = NULL;

        event = clawt_event_new("message.steered", target);
        clawt_event_set_detail(event, "room", clawt_room_get_id(room));
        clawt_event_set_detail_int(
            event, "held",
            (gint64)clawt_steer_queue_pending(self->steers, target));
        clawt_event_bus_publish(self->bus, event);
        g_clear_pointer(&event, clawt_event_free);
        event = clawt_event_new("mailbox.follow-ups-changed", target);
        clawt_event_bus_publish(self->bus, event);
    }

    return TRUE;
}
