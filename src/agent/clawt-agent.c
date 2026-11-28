/*
 * clawt-agent.c - One agent in the fleet
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "agent/clawt-agent.h"

enum {
    SIGNAL_STATE_CHANGED,
    SIGNAL_ERROR,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

struct _ClawtAgent {
    GObject parent_instance;

    ClawtAgentConfig  *config;
    ClawtMailbox      *mailbox;
    ClawtMemoryStore  *memory;

    /* What it is doing right now; see clawt_agent_set_activity(). */
    gboolean           busy;
    gchar             *activity_peer;

    /* On a fallback backend, or held; see clawt_agent_set_failover(). */
    JsonObject        *failover;
    gint64             held_until;   /* Unix seconds; 0 when not held */
    ClawtAgentRuntime *runtime;
    ClawtComputer     *computer;
    ClawtDesktop      *desktop;

    /*
     * Whether @computer still describes the configuration in hand.
     *
     * The computer is derived from the config and built once, at the
     * agent's first start -- and it was kept for the life of the
     * ClawtAgent, which outlives every reload.  So a mount corrected in
     * clawtilla.yaml, saved, reloaded and confirmed by `agent mount
     * list` was still refused at start by the object built from the
     * config before the correction, and the only way out was restarting
     * the daemon, which costs every other agent its turn.
     *
     * A flag rather than comparing config pointers, because `agent set`
     * edits the very object the agent is already holding: the pointer is
     * unchanged and the contents are not.
     */
    gboolean           computer_stale;
    ClawtLink         *link;

    ClawtAgentState state;
    ClawtAgentCaps  caps;

    /*
     * What this turn is answering, and what is waiting behind it.
     *
     * The four live fields describe the turn that is running.  The
     * queue holds one entry per delivery that has not started its turn
     * yet, because libreclaw runs exactly one turn per message --
     * LcSession keeps its own GQueue and drain_next_message() pops a
     * single entry -- so a drain that hands over five messages produces
     * five turns, not one.
     *
     * This was a single gboolean saying "a delivery set the next turn
     * up", spent by the first clawt_agent_begin_turn() after it.  With
     * five deliveries and five turns, turns two through five were
     * therefore treated as turns nothing delivered into: the depth went
     * back to zero, so max_hops could not be reached; turn_replies went
     * back to TRUE, so an acknowledgement carrying invites_reply: 0 was
     * answered anyway; and turn_origin was cleared, which silently
     * disabled clawtilla_message_user()'s guard, so a peer-started turn
     * pushed its findings straight into the operator's chat.  One
     * operator question produced three messages, two of them from
     * somebody else's conversation.
     *
     * turn_task_id came last and for a different symptom: it is the task
     * the delivery belonged to, and it is what clawtilla_delegate uses
     * as the parent of anything the agent hands on from here.  Without
     * it every agent-delegated task was created with parent NULL, so the
     * whole tree was flat -- clawt_task_manager_create()'s depth limit
     * measured zero for every one of them and clawt_task_manager_cancel()
     * found no children to cascade to.  It belongs beside the other
     * three because it is the same kind of fact: true of one delivery,
     * and there are as many of those as were delivered.
     */
    /*
     * One turn per room, because an agent runs one.
     *
     * libreclaw keys a session on channel, room and sender and runs a
     * turn per message in it, and `processing` is per session -- so an
     * agent talking to three peers has three sessions that can each be
     * mid-turn at once.  A live fleet showed exactly that: one agent
     * with three rooms' typing frames overlapping and three separate
     * falses, one per room.
     *
     * These fields were scalars on the agent, so those three turns
     * shared one description and each was judged by whichever room
     * wrote last.  The severe one is the reply flag: a real answer
     * swallowed because *another* room's turn was a closed exchange is
     * silent data loss, and a sign-off delivered because another
     * room's was not costs the recipient a whole model turn.
     *
     * `turns` holds the latest turn in each room and outlives it, since
     * the answer is posted after the indicator drops.  `typing_rooms`
     * is which of them are running, which is what the edge and `busy`
     * are read from.  `last_room` is for a caller that cannot name one:
     * a tool call arrives on a per-agent MCP link with no room on it,
     * so the getters fold across the running turns and each picks the
     * safe direction rather than a plausible one.
     */
    GHashTable     *turns;         /* room -> TurnSetup (owned) */
    GHashTable     *channel_rooms; /* room -> "channel\npeer" (owned) */
    GHashTable     *typing_rooms;  /* room -> gint64*: when its turn began */
    gchar          *last_room;
    guint64         turn_serial;   /* turns begun, for the pair budget */
    GQueue         *pending;       /* TurnSetup (owned), tagged by room */

    gchar          *status_detail;
};

/*
 * One delivery's worth of turn state, waiting for the turn it set up.
 *
 * A scalar cannot describe N messages, and there are N: this is the
 * whole reason the queue exists rather than a counter.  A counter would
 * keep the *last* delivery's origin for all of them, which for a burst
 * of [peer, operator] names the wrong one on both turns.
 */
typedef struct {
    gchar    *room;      /* NULL: any room's next turn may take it */
    gint      depth;
    gboolean  replies;
    gchar    *origin;
    gchar    *task_id;
    ClawtMailboxItem *input; /* The exact delivery, including return context. */

	/*
	 * What the turn did besides write, counted from its streamed tool
	 * steps and its clawtilla tool calls.  Per turn, so it starts at
	 * zero on the rising edge and is still there after the falling one,
	 * when the answer it describes is posted.
	 */
	guint     tool_calls;

	/*
	 * A consultation: the turn is offered no tool that acts.  From the
	 * delivery (clawt_mailbox_item_get_consult()), folded towards TRUE
	 * wherever several deliveries become one turn.
	 */
	gboolean  consult;

	/*
	 * What the turn has done that changes what its closing text is
	 * (since 0.34.2): it already told the operator with a tool, so the
	 * closing text of a turn nobody asked for would say it twice; and
	 * who it asked something of, so closing text that can only say "I
	 * have asked, waiting" is marked as that.
	 */
	gboolean  told_operator;
	gchar    *asked;

	/*
	 * Evidence of work, for the task progress guard: tool calls that
	 * succeeded and were not a progress note, whether any streamed step
	 * arrived at all (an unstreamed turn is no evidence, not zero), and
	 * the last failure's text.
	 */
	guint     work_calls;
	gboolean  evidence;
	gchar    *last_error;

	/*
	 * The mailbox's turn records this turn answers for (gint64 ids):
	 * one per delivery, more when an overflowing queue folded several
	 * into one.  Closed together on the falling edge; marked together
	 * when the turn acts outward.  NULL for a turn nothing recorded --
	 * one no delivery set up, or one handed over with no mailbox.
	 */
	GArray   *records;

	/*
	 * Delivered with a steer request and not yet answered (Since
	 * 0.28.0).  libreclaw has either handed the message to the turn
	 * running in its room or is about to queue it, and only its
	 * chat.steered says which -- so until then no turn may take this
	 * entry: clawt_agent_begin_turn() passes over it, and
	 * clawt_agent_settle_steer() either folds it into the running turn
	 * or releases it to be taken like any other.
	 */
	gboolean  steering;
} TurnSetup;


static void
turn_setup_free(gpointer data)
{
    TurnSetup *setup = data;

    g_free(setup->room);
    g_free(setup->origin);
    g_free(setup->task_id);
	g_free(setup->last_error);
	g_free(setup->asked);
	g_clear_pointer(&setup->input, clawt_mailbox_item_free);
	g_clear_pointer(&setup->records, g_array_unref);
    g_free(setup);
}

static void
queue_of_turns_free(GQueue *queue)
{
    g_queue_free_full(queue, turn_setup_free);
}

/* How far behind an agent may fall; see trim_pending_turns(). */
#define CLAWT_AGENT_MAX_PENDING_TURNS (64)

/* How many rooms' turns are remembered; see trim_turn_history(). */
#define CLAWT_AGENT_MAX_TURN_ROOMS (64)

/*
 * The key a room is filed under.
 *
 * A frame that names no room still has to pair with its own FALSE, and
 * every unnamed frame is the same conversation as far as this can tell.
 * Keyed rather than ignored, because ignoring it would leave an agent
 * that only ever sends unnamed frames with an empty set -- so every one
 * of them would read as a rising edge, which is the bug this exists to
 * stop.
 */
#define CLAWT_AGENT_UNNAMED_ROOM "\x01unnamed"

static const gchar *
room_key(const gchar *room_id)
{
    return (room_id != NULL) ? room_id : CLAWT_AGENT_UNNAMED_ROOM;
}

/*
 * Forgets the room whose turn ran longest ago.
 *
 * The table is keyed by room and an agent can be moved between rooms
 * for as long as it lives, so without a bound it grows for the life of
 * the daemon.  Never a room that is currently typing: that entry is a
 * running turn, and dropping it would leave the answer with nothing to
 * be judged by.
 */
static void
trim_turn_history(ClawtAgent *self)
{
    GHashTableIter iter;
    gpointer key;

    if (g_hash_table_size(self->turns) <= CLAWT_AGENT_MAX_TURN_ROOMS)
        return;

    g_hash_table_iter_init(&iter, self->turns);

    while (g_hash_table_iter_next(&iter, &key, NULL)) {
        if (g_hash_table_contains(self->typing_rooms, key))
            continue;

        if (g_strcmp0(key, self->last_room) == 0)
            continue;

        g_hash_table_iter_remove(&iter);
        return;
    }
}

G_DEFINE_FINAL_TYPE(ClawtAgent, clawt_agent, G_TYPE_OBJECT)

/*
 * Whether an agent in this state can still be part-way through a turn.
 *
 * Everything answering %FALSE has either had its process taken away or is
 * about to, so nothing will finish the turn and nothing will report that
 * it did.  DEGRADED answers %TRUE deliberately: a degraded agent has an
 * unhealthy link and a live process, and its turn may still land.
 */
static gboolean
state_can_be_working(ClawtAgentState state)
{
    switch (state) {
    case CLAWT_AGENT_STATE_STOPPING:
    case CLAWT_AGENT_STATE_STOPPED:
    case CLAWT_AGENT_STATE_ERROR:
    case CLAWT_AGENT_STATE_SHADOW:
        return FALSE;

    case CLAWT_AGENT_STATE_STARTING:
    case CLAWT_AGENT_STATE_RUNNING:
    case CLAWT_AGENT_STATE_DEGRADED:
        return TRUE;
    }

    /*
     * Every state named and no `default:`, so -Wswitch fails the build
     * when one is added rather than sweeping it into "still working" --
     * which is the answer that reintroduces this bug, silently, for
     * whichever state comes next.  Unreachable, and gcc cannot know
     * that.
     */
    return TRUE;
}

static void
set_state(ClawtAgent *self, ClawtAgentState state, const gchar *detail)
{
    if (self->state == state &&
        g_strcmp0(self->status_detail, detail) == 0)
        return;

    self->state = state;

    g_free(self->status_detail);
    self->status_detail = g_strdup(detail);

    /*
     * A turn cannot outlive the process that was taking it.
     *
     * `busy` had one setter -- delivery -- and one clearer: the link
     * reporting typing = FALSE at the end of the turn.  Every route out
     * of RUNNING closes that link, which is precisely what guarantees
     * the message that would have cleared the flag can never arrive.  So
     * an agent stopped or killed mid-turn stayed "working" for the life
     * of the daemon, drawn with a live spinner beside a state dot
     * reading stopped.
     *
     * Here rather than on either stop path, because the two of them do
     * not cover the same ground and neither covers all of it: a killed
     * agent reaches neither clawt_daemon_stop_agent() nor
     * clawt_agent_stop(), and arrives at ERROR through
     * on_runtime_exited() instead.  This is the one line every route
     * passes through, and it is the transition itself rather than any
     * particular way of reaching it that makes the turn impossible.
     *
     * Last before the signal, so a handler that re-reads the agent sees
     * the state, the detail and the activity all agreeing rather than
     * some of each.
     *
     * The peer is kept, because set_activity() preserves it when passed
     * NULL, so agent.list still reports who the last turn was for.  No
     * client draws it once busy is false -- both sidebars show activity
     * only while it is true -- so this is a choice left open rather than
     * a feature: it is the only trace of that turn left once the process
     * is gone, and clearing it would foreclose showing it later at no
     * saving now.
     */
    if (!state_can_be_working(state))
        clawt_agent_set_activity(self, FALSE, NULL);

    g_signal_emit(self, signals[SIGNAL_STATE_CHANGED], 0, state, detail);
}

/*
 * Works out what this agent can actually do, from what it has rather than
 * what its config asked for.
 *
 * This exists so no interface offers a control the agent cannot honour, and
 * so the agent is never told it has a tool that is not really there -- an
 * agent that believes it has a computer it cannot reach burns whole turns
 * hunting for it.
 */
static void
recompute_caps(ClawtAgent *self)
{
    ClawtAgentCaps caps = CLAWT_AGENT_CAPS_NONE;

    /*
     * Every agent in a fleet is served the orchestration tools over its
     * link, whichever runtime it uses and whether or not it is running.
     * These two must not come from the runtime object: a stopped agent
     * would then be unable to say who its peers are, and messaging a
     * stopped agent is the whole point of the mailbox.
     */
    caps |= CLAWT_AGENT_CAPS_TOOLS_MCP | CLAWT_AGENT_CAPS_PEER_COMMS;

    if (self->runtime != NULL)
        caps |= clawt_agent_runtime_get_caps(self->runtime);

    /*
     * Asked of the computer object, not of the config that requested it.
     * A computer that failed to provision leaves the config saying
     * "container" while the agent has nothing, and an agent told it has a
     * computer it cannot reach burns whole turns hunting for it.
     */
    if (self->computer != NULL) {
        ClawtComputerType type =
            clawt_computer_get_computer_type(self->computer);

        if (type != CLAWT_COMPUTER_NONE)
            caps |= CLAWT_AGENT_CAPS_COMPUTER;

        if (type == CLAWT_COMPUTER_HOST || type == CLAWT_COMPUTER_DISPLAY)
            caps |= CLAWT_AGENT_CAPS_HOST_CONTROL;

        /*
         * A display computer is a screen (0.51.0): the agent sees it, and
         * acts on it unless computer.display.allow_input says not.  A
         * private screen nobody sits at, so on by default, unlike the
         * operator's own desktop.
         */
        if (type == CLAWT_COMPUTER_DISPLAY) {
            caps |= CLAWT_AGENT_CAPS_DESKTOP;
            if (clawt_agent_config_get_boolean(self->config,
                                               "computer.display.allow_input"))
                caps |= CLAWT_AGENT_CAPS_DESKTOP_INPUT;
        }
    }

    {
        g_autoptr(GPtrArray) mounts =
            clawt_agent_config_get_mounts(self->config);

        if (mounts != NULL && mounts->len > 0)
            caps |= CLAWT_AGENT_CAPS_MOUNTS;
    }

    if (clawt_agent_config_get_boolean(self->config,
                                       "computer.desktop.enabled")) {
        caps |= CLAWT_AGENT_CAPS_DESKTOP;

        /*
         * Input is a separate capability from seeing the screen.  An agent
         * that can take screenshots but not click is a genuinely useful
         * amount of access and a much smaller grant, so the two are not
         * conflated.
         */
        if (clawt_agent_config_get_boolean(self->config,
                                           "computer.desktop.allow_input"))
            caps |= CLAWT_AGENT_CAPS_DESKTOP_INPUT;
    }

    if (clawt_agent_config_get_string(self->config, "model.effort") != NULL)
        caps |= CLAWT_AGENT_CAPS_EFFORT_LEVELS;

    self->caps = caps;
}

ClawtAgent *
clawt_agent_new(ClawtAgentConfig *config, ClawtMailbox *mailbox)
{
    ClawtAgent *self;

    g_return_val_if_fail(config != NULL, NULL);

    self = g_object_new(CLAWT_TYPE_AGENT, NULL);
    self->config = clawt_agent_config_ref(config);

    if (mailbox != NULL)
        self->mailbox = g_object_ref(mailbox);

    if (clawt_agent_config_is_shadow(config)) {
        self->state = CLAWT_AGENT_STATE_SHADOW;
        self->status_detail =
            g_strdup(clawt_agent_config_get_shadow_reason(config));
    } else {
        self->state = CLAWT_AGENT_STATE_STOPPED;
    }

    recompute_caps(self);

    return self;
}

const gchar *
clawt_agent_get_id(ClawtAgent *self)
{
    g_return_val_if_fail(CLAWT_IS_AGENT(self), NULL);

    return clawt_agent_config_get_id(self->config);
}

const gchar *
clawt_agent_get_name(ClawtAgent *self)
{
    const gchar *name;

    g_return_val_if_fail(CLAWT_IS_AGENT(self), NULL);

    name = clawt_agent_config_get_string(self->config, "name");

    /* An agent with no display name is called by its id rather than nothing. */
    return (name != NULL) ? name : clawt_agent_get_id(self);
}

const gchar *
clawt_agent_get_description(ClawtAgent *self)
{
    g_return_val_if_fail(CLAWT_IS_AGENT(self), NULL);

    return clawt_agent_config_get_string(self->config, "description");
}

ClawtAgentState
clawt_agent_get_state(ClawtAgent *self)
{
    g_return_val_if_fail(CLAWT_IS_AGENT(self), CLAWT_AGENT_STATE_ERROR);

    return self->state;
}

ClawtAgentCaps
clawt_agent_get_caps(ClawtAgent *self)
{
    g_return_val_if_fail(CLAWT_IS_AGENT(self), CLAWT_AGENT_CAPS_NONE);

    return self->caps;
}

ClawtAgentConfig *
clawt_agent_get_config(ClawtAgent *self)
{
    g_return_val_if_fail(CLAWT_IS_AGENT(self), NULL);

    return self->config;
}

ClawtMailbox *
clawt_agent_get_mailbox(ClawtAgent *self)
{
    g_return_val_if_fail(CLAWT_IS_AGENT(self), NULL);

    return self->mailbox;
}

void
clawt_agent_set_memory(ClawtAgent *self, ClawtMemoryStore *memory)
{
    g_return_if_fail(CLAWT_IS_AGENT(self));

    g_clear_object(&self->memory);

    if (memory != NULL)
        self->memory = g_object_ref(memory);
}

ClawtMemoryStore *
clawt_agent_get_memory(ClawtAgent *self)
{
    g_return_val_if_fail(CLAWT_IS_AGENT(self), NULL);

    return self->memory;
}

void
clawt_agent_set_activity(ClawtAgent *self, gboolean busy, const gchar *peer)
{
    g_return_if_fail(CLAWT_IS_AGENT(self));

    self->busy = busy;

    /*
     * The peer is kept when a turn ends rather than cleared, so a
     * finished turn can still say who it was for -- "answered
     * researcher" is worth more than "idle".
     */
    if (peer != NULL) {
        g_free(self->activity_peer);
        self->activity_peer = g_strdup(peer);
    }
}

void
clawt_agent_set_failover(ClawtAgent *self,
                         JsonObject *failover)
{
    gint64 since;
    JsonObject *copy;
    GList *members;
    GList *l;
    gboolean was_held;

    g_return_if_fail(CLAWT_IS_AGENT(self));

    was_held = clawt_failover_is_held(self->failover);

    if (!clawt_failover_is_active(failover) &&
        !clawt_failover_is_held(failover)) {
        g_clear_pointer(&self->failover, json_object_unref);
        self->held_until = 0;

        /*
         * Answered again.  Back to running only from the degraded state
         * the hold put it in, and only with its link up: a hold cleared
         * because the link went is a degraded agent for that reason.
         */
        if (was_held && self->state == CLAWT_AGENT_STATE_DEGRADED &&
            self->link != NULL && clawt_link_is_open(self->link))
            set_state(self, CLAWT_AGENT_STATE_RUNNING, NULL);
        return;
    }

    /*
     * Since the agent first left its primary, not since the latest
     * report: the primary refusing again moves the retry time and sends
     * a fresh frame, and that is not the agent leaving a second time.
     */
    since = (self->failover != NULL)
        ? json_object_get_int_member_with_default(self->failover, "since",
                                                  g_get_real_time())
        : g_get_real_time();

    /* A copy, so nothing the link does to its payload reaches here. */
    copy = json_object_new();
    members = json_object_get_members(failover);
    for (l = members; l != NULL; l = l->next)
        json_object_set_member(copy, l->data,
                               json_node_copy(json_object_get_member(
                                   failover, l->data)));
    g_list_free(members);

    json_object_set_int_member(copy, "since", since);

    g_clear_pointer(&self->failover, json_object_unref);
    self->failover = copy;

    if (!clawt_failover_is_held(copy)) {
        self->held_until = 0;
        return;
    }

    /*
     * Held until the reset the refusal named.  With none -- a login that
     * is gone has none -- until a probe window from now, after which one
     * delivery goes through: its turn is refused again and renews the
     * hold, or it is answered and the agent sends the release.  Holding
     * for ever on no reset would leave nothing able to notice the login
     * came back.
     *
     * A reset more than CLAWT_AGENT_HELD_MAX_SECONDS out is read as that
     * far: the frame is wrong, not the account, and a figure that large
     * also overflowed the wake's arithmetic in daemon-held.c.
     */
    {
        gint64 now = g_get_real_time() / G_USEC_PER_SEC;
        gint64 retry_at = json_object_get_int_member_with_default(
            copy, "retry_at", 0);

        if (retry_at > now + CLAWT_AGENT_HELD_MAX_SECONDS)
            retry_at = now + CLAWT_AGENT_HELD_MAX_SECONDS;

        self->held_until = retry_at > now
            ? retry_at : now + CLAWT_AGENT_HELD_PROBE_SECONDS;
    }

    if (self->state == CLAWT_AGENT_STATE_RUNNING ||
        (self->state == CLAWT_AGENT_STATE_DEGRADED && was_held)) {
        g_autofree gchar *why = g_strdup_printf(
            "held: every turn is refused because it %s",
            clawt_failover_reason_phrase(
                json_object_get_string_member_with_default(copy, "reason",
                                                           NULL)));

        set_state(self, CLAWT_AGENT_STATE_DEGRADED, why);
    }
}

gboolean
clawt_agent_is_turn_held(ClawtAgent *self, gint64 now)
{
    g_return_val_if_fail(CLAWT_IS_AGENT(self), FALSE);

    return clawt_failover_is_held(self->failover) && now < self->held_until;
}

gint64
clawt_agent_get_held_until(ClawtAgent *self)
{
    g_return_val_if_fail(CLAWT_IS_AGENT(self), 0);

    return clawt_failover_is_held(self->failover) ? self->held_until : 0;
}

JsonObject *
clawt_agent_get_failover(ClawtAgent *self)
{
    g_return_val_if_fail(CLAWT_IS_AGENT(self), NULL);

    return self->failover;
}

gboolean
clawt_agent_get_busy(ClawtAgent *self)
{
    g_return_val_if_fail(CLAWT_IS_AGENT(self), FALSE);

    return self->busy;
}

const gchar *
clawt_agent_get_activity_peer(ClawtAgent *self)
{
    g_return_val_if_fail(CLAWT_IS_AGENT(self), NULL);

    return self->activity_peer;
}

/*
 * Deliberately takes the two scalars rather than a #ClawtAgent: the
 * clients that need it hold a JSON object off the wire and have no
 * agent, and a rule both clients apply belongs where it can be tested
 * without a window or a browser.
 */
gchar *
clawt_agent_activity_label(gboolean busy, const gchar *peer)
{
    if (!busy)
        return NULL;

    /*
     * The operator arrives as the literal sender id "user", so this is
     * not a NULL check.  Rendering it would put "working for user" in
     * front of the user.
     */
    if (peer != NULL && *peer != '\0' && g_strcmp0(peer, "user") != 0)
        return g_strdup_printf("working for %s", peer);

    return g_strdup("working");
}

gboolean
clawt_agent_is_chief_of_staff(ClawtAgent *self)
{
    g_return_val_if_fail(CLAWT_IS_AGENT(self), FALSE);

    return clawt_agent_config_get_boolean(self->config, "chief_of_staff");
}

const gchar *
clawt_agent_get_status_detail(ClawtAgent *self)
{
    g_return_val_if_fail(CLAWT_IS_AGENT(self), NULL);

    return self->status_detail;
}

/*
 * The runtime came up -- which is not always because we asked it to.
 *
 * The restart policy respawns the child from inside #ClawtAgentRuntime,
 * so clawt_agent_start() is not on that path and nothing moved the
 * agent out of the state its death put it in.  clawt_agent_set_link()
 * only promotes STARTING and DEGRADED, so a crashed agent that the
 * supervisor had already brought back stayed ERROR for ever: working,
 * because the router drains on the link rather than the state, while
 * every surface reported the crash it had recovered from.  Pressing
 * Start then made it worse -- the runtime answers "the agent is already
 * running" and the failed start writes that in as a fresh error.
 *
 * STARTING rather than RUNNING, because the process being up is not the
 * same as being reachable: the link decides that, exactly as it does on
 * the ordinary start path.
 */
static void
on_runtime_started(ClawtAgentRuntime *runtime, gpointer user_data)
{
    ClawtAgent *self = user_data;

    (void)runtime;

    /*
     * A shadow does not run, and a stop in progress is not undone by
     * the child it is still waiting on.
     */
    if (self->state == CLAWT_AGENT_STATE_SHADOW ||
        self->state == CLAWT_AGENT_STATE_STOPPING)
        return;

    if (self->state != CLAWT_AGENT_STATE_STARTING &&
        self->state != CLAWT_AGENT_STATE_RUNNING)
        set_state(self, CLAWT_AGENT_STATE_STARTING, NULL);
}

static void
on_runtime_exited(ClawtAgentRuntime *runtime,
                  gboolean           clean,
                  const gchar       *detail,
                  gpointer           user_data)
{
    ClawtAgent *self = user_data;

    /*
     * The link goes with the process.  Leaving it attached would let
     * delivery keep succeeding into a socket nobody is reading.
     */
    g_clear_object(&self->link);

    /*
     * And with it every turn the dead process was described by.
     *
     * The typing set first: a runtime that exits mid-turn sends no
     * FALSE, so the set would keep a room for ever -- and the restarted
     * agent's first frame would not read as a turn start, leaving its
     * first turn describing whatever the dead one had been answering.
     *
     * The same sentence is true of the other two, which is why they go
     * with it.  Deliveries are acknowledged at the socket and
     * libreclaw's queue is in memory, so messages handed over and not
     * yet started died with the child; their pending entries would
     * otherwise be popped by the *next* turns in those rooms, and a
     * fresh question would inherit a lost peer exchange's depth, task,
     * origin and -- worst, because it is silent -- its `replies: FALSE`,
     * so the answer is dropped as a sign-off.  And `turns` holds
     * finished turns to judge late answers by, but no answer is coming
     * from a process that is gone; left there, the last one stands in
     * for the agent's depth in clawt_agent_get_hop_depth(), which is
     * what decides whether it is offered the tools that start a hop at
     * all.
     */
    clawt_agent_clear_typing(self);
    g_queue_clear_full(self->pending, (GDestroyNotify)turn_setup_free);
    g_hash_table_remove_all(self->turns);
    g_clear_pointer(&self->last_room, g_free);

    if (self->state == CLAWT_AGENT_STATE_STOPPING || clean)
        set_state(self, CLAWT_AGENT_STATE_STOPPED, detail);
    else
        set_state(self, CLAWT_AGENT_STATE_ERROR, detail);
}

/*
 * The turn running in one room, or the last one that ran there.
 *
 * Kept after the turn ends rather than removed, because the daemon reads
 * it *after* the indicator drops: libreclaw lowers typing in
 * on_process_message_finish() and posts the answer afterwards, so a
 * table that forgot a room on the falling edge would have nothing to
 * judge that answer by.  Replaced on the room's next rising edge.
 */
static TurnSetup *
turn_in(ClawtAgent *self, const gchar *room_id)
{
    return g_hash_table_lookup(self->turns, room_key(room_id));
}

/* The rooms whose turns are running right now. */
static gboolean
room_is_live(ClawtAgent *self, const gchar *room_id)
{
    return g_hash_table_contains(self->typing_rooms, room_key(room_id));
}

/*
 * The turn a caller that cannot name a room is asking about.
 *
 * A tool call arrives on a per-agent MCP link and carries no room, so
 * "which turn is this" has no answer when several are running.  The
 * folds below each pick the safe direction rather than a plausible one;
 * this picks the turn to fall back to when nothing is running at all,
 * which is the ordinary case for a test that set a field and read it.
 */
static TurnSetup *
latest_turn(ClawtAgent *self)
{
    if (self->last_room != NULL)
        return g_hash_table_lookup(self->turns, self->last_room);

    return NULL;
}

const gchar *
clawt_agent_get_last_turn_room(ClawtAgent *self)
{
    g_return_val_if_fail(CLAWT_IS_AGENT(self), NULL);

    /*
     * The unnamed sentinel is a bookkeeping key, not a room anybody can
     * route to; a caller asking "which room" must hear "none" for it.
     */
    if (self->last_room == NULL ||
        g_strcmp0(self->last_room, CLAWT_AGENT_UNNAMED_ROOM) == 0)
        return NULL;

    return self->last_room;
}

gint
clawt_agent_get_hop_depth(ClawtAgent *self)
{
    GHashTableIter iter;
    gpointer key;
    gpointer value;
    gint deepest = -1;

    g_return_val_if_fail(CLAWT_IS_AGENT(self), 0);

    /*
     * The deepest of the turns actually running, not the most recent.
     *
     * A caller with no room to name gets the answer that errs towards
     * refusing: `orchestration.max_hops` firing one hop early on an
     * agent that is mid-conversation in two rooms costs a delegation,
     * where firing late costs the runaway this whole area exists to
     * stop.
     */
    g_hash_table_iter_init(&iter, self->turns);

    while (g_hash_table_iter_next(&iter, &key, &value)) {
        TurnSetup *setup = value;

        if (!g_hash_table_contains(self->typing_rooms, key))
            continue;

        if (setup->depth > deepest)
            deepest = setup->depth;
    }

    if (deepest >= 0)
        return deepest;

    {
        TurnSetup *setup = latest_turn(self);

        if (setup != NULL)
            return setup->depth;
    }

    {
        TurnSetup *next = g_queue_peek_head(self->pending);

        return (next != NULL) ? next->depth : 0;
    }
}

gint
clawt_agent_get_hop_depth_in(ClawtAgent *self, const gchar *room_id)
{
    TurnSetup *setup;

    g_return_val_if_fail(CLAWT_IS_AGENT(self), 0);

    setup = turn_in(self, room_id);

    return (setup != NULL) ? setup->depth : clawt_agent_get_hop_depth(self);
}

/*
 * Folds the oldest waiting delivery into the one behind it.
 *
 * The queue is fed by whatever a peer sends and drained by turns that
 * each cost a model call, so the producer can outrun the consumer
 * indefinitely.  Merged rather than dropped: dropping loses a close
 * signal, and an agent that never learns an exchange is closed answers
 * it, which is the loop this whole mechanism exists to end.  The merge
 * keeps the deeper hop count and the more restrictive reply flag, so
 * overflow errs towards stopping an exchange rather than continuing one.
 *
 * Into the next entry for the *same room* where there is one, since that
 * is the turn the close signal is about.  Failing that, into whatever is
 * behind it -- still better than dropping the flag.
 *
 * The task id is the exception and is never carried across.  It names
 * one particular piece of work rather than describing a disposition, so
 * inheriting it would attribute a later message to somebody else's task
 * -- and the daemon completes a task from the message that ends its
 * turn, so the wrong task would be marked done by a message that was
 * never about it.  A task that ends late is a delay; one that ends early
 * is a lie.
 */
static void
trim_pending_turns(ClawtAgent *self)
{
    TurnSetup *oldest;
    TurnSetup *next = NULL;
    guint i;

    if (g_queue_get_length(self->pending) <= CLAWT_AGENT_MAX_PENDING_TURNS)
        return;

    oldest = g_queue_pop_head(self->pending);

    for (i = 0; i < g_queue_get_length(self->pending); i++) {
        TurnSetup *candidate = g_queue_peek_nth(self->pending, i);

        if (g_strcmp0(candidate->room, oldest->room) == 0) {
            next = candidate;
            break;
        }
    }

    if (next == NULL)
        next = g_queue_peek_head(self->pending);

    if (oldest != NULL && next != NULL) {
        if (oldest->depth > next->depth)
            next->depth = oldest->depth;

        next->replies = next->replies && oldest->replies;

        /* A folded consultation keeps the turn a consultation. */
        next->consult = next->consult || oldest->consult;

        if (next->origin == NULL)
            next->origin = g_strdup(oldest->origin);

		/*
		 * And its turn records.  The folded message is still in
		 * libreclaw's queue and will still run; only its description
		 * went.  Left behind, its record would never be closed, and the
		 * next restart would rerun a message that had been answered.
		 */
		if (oldest->records != NULL) {
			if (next->records == NULL)
				next->records = g_array_new(FALSE, FALSE, sizeof(gint64));
			g_array_append_vals(next->records, oldest->records->data,
				oldest->records->len);
		}
    }

    g_warning("agent %s: more than %d messages are waiting for a turn; the "
              "oldest has been folded into the one behind it",
              clawt_agent_get_id(self), CLAWT_AGENT_MAX_PENDING_TURNS);

    turn_setup_free(oldest);
}

/*
 * The entry the delivery being described is filling in.
 *
 * The four setters are called one after another for a single item, so
 * they all write the same tail entry; a setter reached with an empty
 * queue starts one.  That is what makes each of them arm the queue on
 * its own -- clawt_agent_set_turn_origin() did not arm the old boolean,
 * which meant a caller that set only the origin had it discarded by the
 * next clawt_agent_begin_turn(), silently, on the path about to read it.
 *
 * An entry made this way carries no room, so the next turn in *any* room
 * takes it.  That is what a caller who never named one is asking for,
 * and in production nothing is in that position: the router always has
 * the room, because it is the thing that chose it.
 */
static TurnSetup *
pending_turn(ClawtAgent *self)
{
    TurnSetup *setup = g_queue_peek_tail(self->pending);

    if (setup != NULL)
        return setup;

    setup = g_new0(TurnSetup, 1);
    setup->replies = TRUE;

    g_queue_push_tail(self->pending, setup);

    return setup;
}

void
clawt_agent_deliver_turn(ClawtAgent  *self,
                         const gchar *room_id,
                         gint         depth,
                         gboolean     replies,
                         const gchar *from,
                         const gchar *task_id)
{
    TurnSetup *setup;

    g_return_if_fail(CLAWT_IS_AGENT(self));

    /*
     * A whole delivery at once, which is what makes it one entry.
     *
     * The setters below amend the entry at the tail, so a caller
     * describing one delivery a field at a time gets one entry -- and a
     * caller describing three deliveries that way would get one entry
     * for all three, with no way to tell where one ended.  There is no
     * end-of-delivery edge to find; there is only this call.
     *
     * Tagged with the room, so the turn that takes it is the turn in
     * that room.  Untagged, a burst across two rooms was drained in
     * arrival order by whichever room happened to start a turn first,
     * and an agent answering two peers at once described each turn with
     * the other one's message.
     */
    setup = g_new0(TurnSetup, 1);
    setup->room = g_strdup(room_id);
    setup->depth = depth;
    setup->replies = replies;
    setup->origin = g_strdup(from);
    setup->task_id = g_strdup(task_id);

    g_queue_push_tail(self->pending, setup);
    trim_pending_turns(self);
}

void
clawt_agent_set_hop_depth(ClawtAgent *self, gint depth)
{
    g_return_if_fail(CLAWT_IS_AGENT(self));

    pending_turn(self)->depth = depth;
}

gboolean
clawt_agent_get_turn_replies(ClawtAgent *self)
{
    TurnSetup *setup;

    g_return_val_if_fail(CLAWT_IS_AGENT(self), TRUE);

    /*
     * The most recent turn, not a fold.
     *
     * There is no safe direction here: answering FALSE across the board
     * swallows a real reply that another room is waiting for, and TRUE
     * routes a sign-off nobody asked for.  What decides routing is
     * clawt_agent_get_turn_replies_in(), which has the room -- this is
     * for a reader with none, and it says so.
     */
    setup = latest_turn(self);

    if (setup != NULL)
        return setup->replies;

    setup = g_queue_peek_head(self->pending);

    return (setup != NULL) ? setup->replies : TRUE;
}

gboolean
clawt_agent_get_turn_replies_in(ClawtAgent *self, const gchar *room_id)
{
    TurnSetup *setup;

    g_return_val_if_fail(CLAWT_IS_AGENT(self), TRUE);

    setup = turn_in(self, room_id);

    return (setup != NULL) ? setup->replies
                           : clawt_agent_get_turn_replies(self);
}

void
clawt_agent_close_turn_exchange(ClawtAgent *self, const gchar *room_id)
{
    TurnSetup *setup;

    g_return_if_fail(CLAWT_IS_AGENT(self));

    if (room_id == NULL)
        return;

    /*
     * Only the entry that exists.  The pending queue is deliberately
     * left alone: those describe deliveries not yet taken, each with an
     * invite of its own, and spending a *future* turn's invite would
     * swallow the answer to a question the delegator has yet to ask.
     * The entry in the table dies at the room's next rising edge, which
     * is what scopes this to the turn that settled the task.
     */
    setup = turn_in(self, room_id);

    if (setup != NULL)
        setup->replies = FALSE;
}

/* How many foreign rooms are remembered; the oldest go when it is full. */
#define CHANNEL_ROOMS_MAX (64)

void
clawt_agent_note_channel_room(ClawtAgent *self, const gchar *room_id,
                              const gchar *channel, const gchar *peer)
{
    g_return_if_fail(CLAWT_IS_AGENT(self));

    if (room_id == NULL || channel == NULL || *channel == '\0')
        return;

    /*
     * Bounded, and crudely: an agent in more rooms than this at once is
     * not a case worth an eviction order, and forgetting one costs only
     * an answer arriving in the two agents' room instead.
     */
    if (g_hash_table_size(self->channel_rooms) >= CHANNEL_ROOMS_MAX &&
        !g_hash_table_contains(self->channel_rooms, room_id))
        g_hash_table_remove_all(self->channel_rooms);

    g_hash_table_replace(self->channel_rooms, g_strdup(room_id),
                         g_strdup_printf("%s\n%s", channel,
                                         peer != NULL ? peer : ""));
}

gboolean
clawt_agent_get_channel_room(ClawtAgent *self, const gchar *room_id,
                             gchar **channel, gchar **peer)
{
    const gchar *known;
    const gchar *split;

    g_return_val_if_fail(CLAWT_IS_AGENT(self), FALSE);

    known = (room_id != NULL)
        ? g_hash_table_lookup(self->channel_rooms, room_id) : NULL;

    if (known == NULL)
        return FALSE;

    split = strchr(known, '\n');

    if (channel != NULL)
        *channel = g_strndup(known, split != NULL ? (gsize)(split - known)
                                                  : strlen(known));
    if (peer != NULL)
        *peer = (split != NULL && split[1] != '\0') ? g_strdup(split + 1)
                                                    : NULL;

    return TRUE;
}

void
clawt_agent_note_turn_told_operator(ClawtAgent *self, const gchar *room_id)
{
    TurnSetup *setup;

    g_return_if_fail(CLAWT_IS_AGENT(self));

    setup = (room_id != NULL) ? turn_in(self, room_id) : NULL;

    if (setup != NULL)
        setup->told_operator = TRUE;
}

gboolean
clawt_agent_get_turn_told_operator(ClawtAgent *self, const gchar *room_id)
{
    TurnSetup *setup;

    g_return_val_if_fail(CLAWT_IS_AGENT(self), FALSE);

    setup = (room_id != NULL) ? turn_in(self, room_id) : NULL;

    return setup != NULL && setup->told_operator;
}

void
clawt_agent_note_turn_asked(ClawtAgent *self, const gchar *room_id,
                            const gchar *peer)
{
    TurnSetup *setup;

    g_return_if_fail(CLAWT_IS_AGENT(self));

    setup = (room_id != NULL) ? turn_in(self, room_id) : NULL;

    if (setup == NULL || peer == NULL)
        return;

    g_free(setup->asked);
    setup->asked = g_strdup(peer);
}

const gchar *
clawt_agent_get_turn_asked(ClawtAgent *self, const gchar *room_id)
{
    TurnSetup *setup;

    g_return_val_if_fail(CLAWT_IS_AGENT(self), NULL);

    setup = (room_id != NULL) ? turn_in(self, room_id) : NULL;

    return (setup != NULL) ? setup->asked : NULL;
}

/*
 * Who a delivery's turn is for and what task it is about.  Only a
 * correlated, closed answer restores the requester's responsibility; an
 * incoming question is still the peer's ask.  One function because
 * clawt_agent_deliver_item() and the merge both need it, and two copies
 * would disagree about whose turn a correlated answer starts.
 */
static void
item_turn_owner(
	ClawtMailboxItem  *item,
	const gchar      **out_origin,
	const gchar      **out_task
){
	*out_origin = clawt_mailbox_item_get_from(item);
	*out_task = clawt_mailbox_item_get_task_id(item);

	if (!clawt_mailbox_item_get_invites_reply(item) &&
		clawt_mailbox_item_get_reply_to(item) != NULL &&
		clawt_mailbox_item_get_request_room(item) != NULL) {
		*out_origin = clawt_mailbox_item_get_request_origin(item);
		*out_task = clawt_mailbox_item_get_request_task(item);
	}
}

/*
 * Keep the exact input with its turn. Only a correlated, closed answer
 * restores responsibility; an incoming question is still the peer's ask.
 */
/*
 * Writes the mailbox's record of a delivery the agent has just been
 * handed, and files its id on the turn it set up.
 *
 * Only with a mailbox and real item ids: a test that hands an agent a
 * delivery with neither has no queue to hand anything back to.  A write
 * that fails is a warning and not a refused delivery -- the message has
 * already reached the socket, and the worst a missing record costs is
 * that a restart does not rerun this one turn.
 */
static void
open_turn_record(
	ClawtAgent          *self,
	TurnSetup           *setup,
	const gchar         *room_id,
	const gchar * const *ids,
	guint                replays
){
	g_autoptr(GError) error = NULL;
	gint64 record;

	if (self->mailbox == NULL || ids == NULL || ids[0] == NULL)
		return;

	record = clawt_mailbox_turn_open(self->mailbox, room_id, ids, replays,
		&error);

	if (record <= 0) {
		g_warning("agent %s: could not record the turn a delivery started, "
			"so a restart will not rerun it: %s", clawt_agent_get_id(self),
			error != NULL ? error->message : "unknown");
		return;
	}

	if (setup->records == NULL)
		setup->records = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_array_append_val(setup->records, record);
}

/* Closes every record a finished turn answered for. */
static void
close_turn_records(
	ClawtAgent *self,
	TurnSetup  *setup
){
	guint i;

	if (setup == NULL || setup->records == NULL || self->mailbox == NULL)
		return;

	for (i = 0; i < setup->records->len; i++) {
		g_autoptr(GError) error = NULL;

		if (!clawt_mailbox_turn_close(self->mailbox,
				g_array_index(setup->records, gint64, i), &error))
			g_warning("agent %s: could not close a turn record, so a "
				"restart may rerun a finished turn: %s",
				clawt_agent_get_id(self),
				error != NULL ? error->message : "unknown");
	}

	g_array_set_size(setup->records, 0);
}

/* Marks every record of one turn; returns how many there were. */
static guint
mark_turn_records(
	ClawtAgent  *self,
	TurnSetup   *setup,
	const gchar *what
){
	guint marked = 0;
	guint i;

	if (setup == NULL || setup->records == NULL || self->mailbox == NULL)
		return 0;

	/*
	 * A consultation never acts outward: every tool that could is
	 * withheld from it and refused if named (clawt_mcp_tools_consult_
	 * withholds()), so a mark reaching one came from the agent-wide fold
	 * on behalf of another turn.  Left unmarked, a consultation cut by a
	 * restart is rerun once -- which only asks the question again.
	 */
	if (setup->consult)
		return 0;

	for (i = 0; i < setup->records->len; i++) {
		g_autoptr(GError) error = NULL;

		if (clawt_mailbox_turn_mark_outward(self->mailbox,
				g_array_index(setup->records, gint64, i), what, &error))
			marked++;
		else
			g_warning("agent %s: could not mark a turn as having acted "
				"outward, so a restart could repeat %s: %s",
				clawt_agent_get_id(self), what,
				error != NULL ? error->message : "unknown");
	}

	return marked;
}

guint
clawt_agent_mark_outward(
	ClawtAgent  *self,
	const gchar *room_id,
	const gchar *what
){
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	TurnSetup *setup;
	guint marked = 0;

	g_return_val_if_fail(CLAWT_IS_AGENT(self), 0);
	g_return_val_if_fail(what != NULL, 0);

	/*
	 * The turn in the named room, when that room has one running.  A
	 * room that is not running is a description outliving its turn (or
	 * a turn_room an agent made up), and is no better than no room.
	 */
	if (room_id != NULL && room_is_live(self, room_id)) {
		setup = turn_in(self, room_id);
		if (setup != NULL)
			return mark_turn_records(self, setup, what);
	}

	/*
	 * Otherwise every running turn, which is the safe direction and
	 * the only one: a turn wrongly marked ends as "interrupted" after a
	 * restart and costs a person a re-ask, while a turn wrongly left
	 * unmarked is rerun and sends whatever it sent a second time.
	 */
	g_hash_table_iter_init(&iter, self->turns);
	while (g_hash_table_iter_next(&iter, &key, &value))
		if (g_hash_table_contains(self->typing_rooms, key))
			marked += mark_turn_records(self, value, what);

	/*
	 * And with nothing running at all -- a tool call that beat its own
	 * typing frame, or a test -- the turn last begun.
	 */
	if (marked == 0 && g_hash_table_size(self->typing_rooms) == 0)
		marked = mark_turn_records(self, latest_turn(self), what);

	return marked;
}

void
clawt_agent_deliver_item(ClawtAgent *self, ClawtMailboxItem *item,
	gboolean replies)
{
	const gchar *origin;
	const gchar *task_id;
	TurnSetup *setup;

	g_return_if_fail(CLAWT_IS_AGENT(self));
	g_return_if_fail(item != NULL);
	item_turn_owner(item, &origin, &task_id);
	clawt_agent_deliver_turn(self, clawt_mailbox_item_get_room(item),
		clawt_mailbox_item_get_depth(item), replies, origin, task_id);
	setup = g_queue_peek_tail(self->pending);
	setup->input = clawt_mailbox_item_copy(item);
	setup->consult = clawt_mailbox_item_get_consult(item);

	{
		const gchar *ids[2] = { clawt_mailbox_item_get_id(item), NULL };

		open_turn_record(self, setup, clawt_mailbox_item_get_room(item),
			ids, clawt_mailbox_item_get_replays(item));
	}
}

void
clawt_agent_deliver_merged(
	ClawtAgent       *self,
	ClawtMailboxItem *delivered,
	GPtrArray        *items,
	const gboolean   *replies,
	const gboolean   *peers
){
	const gchar *origin = NULL;
	const gchar *task_id = NULL;
	gboolean any_replies = FALSE;
	gboolean peer_origin = FALSE;
	gboolean tasks_agree = TRUE;
	gint depth = 0;
	gboolean consult = FALSE;
	TurnSetup *setup;
	guint i;

	g_return_if_fail(CLAWT_IS_AGENT(self));
	g_return_if_fail(delivered != NULL);
	g_return_if_fail(items != NULL && items->len > 0);
	g_return_if_fail(replies != NULL && peers != NULL);

	/*
	 * The one place several deliveries' turn state is folded into one,
	 * and every field in the direction that errs towards stopping:
	 *
	 *   depth    the deepest, so max_hops is not bought back by merging
	 *   replies  TRUE if any part may be answered -- swallowing a real
	 *            answer is silent data loss, one extra answer is a turn
	 *   task     only when every part names the same one (NULL counts):
	 *            a wrong parent hangs work under somebody else's job,
	 *            no parent is what an ambiguous turn gets elsewhere
	 *   origin   any peer's, so clawtilla_message_user's guard holds
	 *            when one part of the turn was a peer's
	 *
	 * The agent-wide getters fold *running* turns the same way; this is
	 * the same rule applied before the turn starts, because after the
	 * merge there is only one turn to describe.
	 */
	for (i = 0; i < items->len; i++) {
		ClawtMailboxItem *item = g_ptr_array_index(items, i);
		const gchar *item_origin;
		const gchar *item_task;

		item_turn_owner(item, &item_origin, &item_task);

		depth = MAX(depth, clawt_mailbox_item_get_depth(item));
		any_replies = any_replies || replies[i];
		/* Towards withholding: one consulted part makes the turn one. */
		consult = consult || clawt_mailbox_item_get_consult(item);

		if (i == 0)
			task_id = item_task;
		else if (g_strcmp0(task_id, item_task) != 0)
			tasks_agree = FALSE;

		/*
		 * A peer's part whose turn belongs to a peer.  A correlated
		 * answer restores its requester, which may be the operator or
		 * nobody the daemon can name; that origin is a fallback, never
		 * preferred over a part that is a peer's business.
		 */
		if (peers[i] && item_origin != NULL && !peer_origin &&
			g_strcmp0(item_origin, "user") != 0 &&
			g_strcmp0(item_origin, CLAWT_SYSTEM_SENDER) != 0) {
			origin = item_origin;
			peer_origin = TRUE;
		} else if (origin == NULL && item_origin != NULL) {
			origin = item_origin;
		}
	}

	clawt_agent_deliver_turn(self, clawt_mailbox_item_get_room(delivered),
		depth, any_replies, origin, tasks_agree ? task_id : NULL);
	setup = g_queue_peek_tail(self->pending);
	setup->input = clawt_mailbox_item_copy(delivered);
	setup->consult = consult;

	/*
	 * One record naming every part, with the highest replay count among
	 * them: the turn is one turn, and a part that was already rerun
	 * once must not buy the whole merge a second rerun.
	 */
	{
		g_autoptr(GPtrArray) ids = g_ptr_array_new();
		guint replays = 0;

		for (i = 0; i < items->len; i++) {
			ClawtMailboxItem *item = g_ptr_array_index(items, i);

			if (clawt_mailbox_item_get_id(item) != NULL)
				g_ptr_array_add(ids, (gpointer)clawt_mailbox_item_get_id(item));
			replays = MAX(replays, clawt_mailbox_item_get_replays(item));
		}
		g_ptr_array_add(ids, NULL);

		open_turn_record(self, setup, clawt_mailbox_item_get_room(delivered),
			(const gchar * const *)ids->pdata, replays);
	}
}

/* ── Steering (Since 0.28.0) ─────────────────────────────────────── */

/* The waiting delivery whose input is @item_id, and where it stands. */
static TurnSetup *
pending_for_item(
	ClawtAgent  *self,
	const gchar *item_id,
	guint       *out_index
){
	guint i;

	if (item_id == NULL)
		return NULL;

	for (i = 0; i < g_queue_get_length(self->pending); i++) {
		TurnSetup *candidate = g_queue_peek_nth(self->pending, i);

		if (candidate->input != NULL &&
			g_strcmp0(clawt_mailbox_item_get_id(candidate->input),
				item_id) == 0) {
			if (out_index != NULL)
				*out_index = i;
			return candidate;
		}
	}

	return NULL;
}

gboolean
clawt_agent_await_steer(
	ClawtAgent  *self,
	const gchar *item_id
){
	TurnSetup *setup;

	g_return_val_if_fail(CLAWT_IS_AGENT(self), FALSE);

	setup = pending_for_item(self, item_id, NULL);
	if (setup == NULL)
		return FALSE;

	setup->steering = TRUE;
	return TRUE;
}

gint64
clawt_agent_get_turn_record_in(
	ClawtAgent  *self,
	const gchar *room_id
){
	TurnSetup *setup;

	g_return_val_if_fail(CLAWT_IS_AGENT(self), 0);

	if (room_id == NULL || !room_is_live(self, room_id))
		return 0;

	setup = turn_in(self, room_id);
	if (setup == NULL || setup->records == NULL || setup->records->len == 0)
		return 0;

	return g_array_index(setup->records, gint64, 0);
}

gboolean
clawt_agent_get_turn_steer_facts(
	ClawtAgent       *self,
	const gchar      *room_id,
	const gchar     **origin,
	const gchar     **task_id,
	gboolean         *consult
){
	TurnSetup *setup;

	g_return_val_if_fail(CLAWT_IS_AGENT(self), FALSE);

	/* Only a turn that is running: a finished one has nothing to steer. */
	if (room_id == NULL || !room_is_live(self, room_id))
		return FALSE;

	setup = turn_in(self, room_id);
	if (setup == NULL)
		return FALSE;

	if (origin != NULL)
		*origin = setup->origin;
	if (task_id != NULL)
		*task_id = setup->task_id;
	if (consult != NULL)
		*consult = setup->consult;

	return TRUE;
}

/*
 * Folds a steered delivery's description into the turn it joined.
 *
 * Every field in the direction that errs towards stopping, as
 * clawt_agent_deliver_merged() folds several deliveries before a turn --
 * this is the same rule applied after the turn has started:
 *
 *   depth    the deepest, so max_hops is not bought back by steering
 *   replies  TRUE if either may be answered: the turn's closing text now
 *            answers both, and swallowing a real answer is silent loss
 *   origin   a peer's over nobody's or the operator's, so
 *            clawtilla_message_user's guard holds once a peer's words
 *            are part of the turn
 *   task     kept when the steer names none or the same one, and dropped
 *            when they differ: a wrong parent hangs work under somebody
 *            else's job, and a turn completing a task it was never about
 *            is a lie where one completing late is a delay
 *   consult  TRUE if either is; the policy never steers across the line,
 *            so this only ever holds a consultation a consultation
 *
 * The turn's input -- the delivery that started it, whose round and
 * thread its answer continues -- is the running turn's and is kept.
 */
static void
fold_steer_into(
	TurnSetup *into,
	TurnSetup *steer
){
	if (steer->depth > into->depth)
		into->depth = steer->depth;

	into->replies = into->replies || steer->replies;
	into->consult = into->consult || steer->consult;

	if (steer->origin != NULL &&
		g_strcmp0(steer->origin, "user") != 0 &&
		g_strcmp0(steer->origin, CLAWT_SYSTEM_SENDER) != 0 &&
		(into->origin == NULL ||
		 g_strcmp0(into->origin, "user") == 0 ||
		 g_strcmp0(into->origin, CLAWT_SYSTEM_SENDER) == 0)) {
		g_free(into->origin);
		into->origin = g_strdup(steer->origin);
	} else if (into->origin == NULL) {
		into->origin = g_strdup(steer->origin);
	}

	if (steer->task_id != NULL &&
		g_strcmp0(steer->task_id, into->task_id) != 0)
		g_clear_pointer(&into->task_id, g_free);
}

/*
 * The steered delivery's turn record joins the running turn's, so a
 * restart that cuts the turn hands both back as one -- or ends both as
 * interrupted -- and never reruns the steered words as a turn of their
 * own after the turn that already read them was rerun.
 */
static void
join_turn_records(
	ClawtAgent *self,
	TurnSetup  *into,
	TurnSetup  *steer
){
	guint i;

	if (steer->records == NULL || steer->records->len == 0)
		return;

	if (into->records == NULL || into->records->len == 0 ||
		self->mailbox == NULL) {
		if (into->records == NULL)
			into->records = g_array_new(FALSE, FALSE, sizeof(gint64));
		g_array_append_vals(into->records, steer->records->data,
			steer->records->len);
		g_array_set_size(steer->records, 0);
		return;
	}

	for (i = 0; i < steer->records->len; i++) {
		g_autoptr(GError) error = NULL;
		gint64 from = g_array_index(steer->records, gint64, i);

		if (!clawt_mailbox_turn_join(self->mailbox,
				g_array_index(into->records, gint64, 0), from, &error)) {
			g_warning("agent %s: a steered message's turn record could not "
				"join the running turn's, so it is kept apart: %s",
				clawt_agent_get_id(self),
				error != NULL ? error->message : "unknown");
			g_array_append_val(into->records, from);
		}
	}

	g_array_set_size(steer->records, 0);
}

ClawtSteerSettle
clawt_agent_settle_steer(
	ClawtAgent  *self,
	const gchar *room_id,
	const gchar *item_id,
	const gchar *outcome,
	gboolean     asked
){
	TurnSetup *steer;
	TurnSetup *into;
	guint index = 0;

	g_return_val_if_fail(CLAWT_IS_AGENT(self), CLAWT_STEER_SETTLE_UNKNOWN);
	g_return_val_if_fail(outcome != NULL, CLAWT_STEER_SETTLE_UNKNOWN);

	steer = pending_for_item(self, item_id, &index);

	/*
	 * Already gone: the agent ran it as a turn of its own before its
	 * answer arrived (clawt_agent_begin_turn()'s last resort), or a
	 * restart cleared the queue.  Nothing left to describe.
	 */
	if (steer == NULL)
		return CLAWT_STEER_SETTLE_UNKNOWN;

	if (g_strcmp0(outcome, "refused") == 0) {
		/*
		 * Provably not delivered: libreclaw queued it, and its own turn
		 * will take this entry like any other.  Where it now stands is
		 * libreclaw's queue order.  A refusal decided before asking
		 * queued it the moment it arrived, which is where it already is;
		 * the engine's own refusal queued it behind everything that
		 * arrived meanwhile, which is the tail.
		 */
		steer->steering = FALSE;

		if (asked) {
			g_queue_pop_nth(self->pending, index);
			g_queue_push_tail(self->pending, steer);
		}

		return CLAWT_STEER_SETTLE_QUEUED;
	}

	/*
	 * Delivered, or indeterminate -- which counts as delivered.  The
	 * message starts no turn, so its description must not wait to be
	 * taken by one: left queued, the next real turn in this room would
	 * pop it and inherit its replies flag, its depth and its task, the
	 * bug class on_runtime_exited() had to clear.  It joins the turn it
	 * was steered into, which is the one this room has on record.
	 */
	g_queue_pop_nth(self->pending, index);
	into = turn_in(self, room_id != NULL ? room_id : steer->room);

	if (into == NULL) {
		/* No turn on record to join: nothing will close its records. */
		close_turn_records(self, steer);
		turn_setup_free(steer);
		return CLAWT_STEER_SETTLE_JOINED;
	}

	fold_steer_into(into, steer);

	/*
	 * A turn that has already ended (an indeterminate answer can come
	 * after the falling edge) closed its records then, so the steered
	 * part's are closed here: the turn read the words, and a restart has
	 * nothing of it to rerun.
	 */
	if (room_is_live(self, into->room))
		join_turn_records(self, into, steer);
	else
		close_turn_records(self, steer);

	turn_setup_free(steer);
	return CLAWT_STEER_SETTLE_JOINED;
}

gboolean
clawt_agent_get_turn_consult_in(
	ClawtAgent  *self,
	const gchar *room_id
){
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	guint i;

	g_return_val_if_fail(CLAWT_IS_AGENT(self), FALSE);

	if (room_id != NULL) {
		TurnSetup *setup;

		/* The turn running there is the answer. */
		if (room_is_live(self, room_id)) {
			setup = turn_in(self, room_id);
			return setup != NULL && setup->consult;
		}

		/*
		 * Nothing running there yet: the delivery its next turn will
		 * take.  The CLI can ask for its tool list before the typing
		 * frame that begins the turn reaches the daemon, and the turn
		 * that list is for is the one this entry describes.
		 */
		for (i = 0; i < g_queue_get_length(self->pending); i++) {
			TurnSetup *candidate = g_queue_peek_nth(self->pending, i);

			if (candidate->room == NULL ||
				g_strcmp0(room_key(candidate->room), room_key(room_id)) == 0)
				return candidate->consult;
		}
	}

	/*
	 * No room, or one with nothing running and nothing waiting: any
	 * running consultation.  The direction that withholds, as the hop
	 * depth's fold is the deepest -- an agent mid-consultation in one
	 * room loses its acting tools in another for as long as both run,
	 * which costs a call; the other way round a consultation could act.
	 * A finished turn is not consulted at all: the tools written into
	 * TOOLS.org at a start must not depend on what the last turn was.
	 */
	g_hash_table_iter_init(&iter, self->turns);

	while (g_hash_table_iter_next(&iter, &key, &value)) {
		if (!g_hash_table_contains(self->typing_rooms, key))
			continue;
		if (((TurnSetup *)value)->consult)
			return TRUE;
	}

	return FALSE;
}

void
clawt_agent_note_turn_work(
	ClawtAgent  *self,
	const gchar *room_id,
	gboolean     ok,
	const gchar *error
){
	GHashTableIter iter;
	gpointer key;
	gpointer value;

	g_return_if_fail(CLAWT_IS_AGENT(self));

	/*
	 * Charged the way clawt_agent_note_turn_tool() charges a call: to
	 * the named room's turn, or to every running turn when the call
	 * names none.  Work credited to a turn that did none makes the
	 * guard late; work withheld from one that did makes it stop a task
	 * that was moving, which is the worse of the two.
	 */
	g_hash_table_iter_init(&iter, self->turns);

	while (g_hash_table_iter_next(&iter, &key, &value)) {
		TurnSetup *setup = value;

		if (room_id != NULL
			? g_strcmp0(key, room_key(room_id)) != 0
			: !g_hash_table_contains(self->typing_rooms, key))
			continue;

		setup->evidence = TRUE;

		if (ok) {
			setup->work_calls++;
		} else if (error != NULL) {
			g_free(setup->last_error);
			setup->last_error = g_strdup(error);
		}
	}
}

gboolean
clawt_agent_get_turn_evidence_in(
	ClawtAgent   *self,
	const gchar  *room_id,
	guint        *work_calls,
	const gchar **last_error
){
	TurnSetup *setup;

	g_return_val_if_fail(CLAWT_IS_AGENT(self), FALSE);

	setup = turn_in(self, room_id);

	if (work_calls != NULL)
		*work_calls = setup != NULL ? setup->work_calls : 0;
	if (last_error != NULL)
		*last_error = setup != NULL ? setup->last_error : NULL;

	/* A turn with neither a step nor a call is unknown, never "none". */
	return setup != NULL && (setup->evidence || setup->work_calls > 0);
}

void
clawt_agent_note_turn_tool(
	ClawtAgent  *self,
	const gchar *room_id
){
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	gboolean any = FALSE;

	g_return_if_fail(CLAWT_IS_AGENT(self));

	if (room_id != NULL) {
		TurnSetup *setup = turn_in(self, room_id);

		if (setup != NULL)
			setup->tool_calls++;
		return;
	}

	/*
	 * No room: a clawtilla tool call arrives on the agent's link with
	 * none.  Every running turn is charged with it, which is the safe
	 * direction -- a turn wrongly credited with a tool call wakes its
	 * asker as it always did, and one wrongly left at zero might not.
	 */
	g_hash_table_iter_init(&iter, self->turns);

	while (g_hash_table_iter_next(&iter, &key, &value)) {
		if (!g_hash_table_contains(self->typing_rooms, key))
			continue;
		((TurnSetup *)value)->tool_calls++;
		any = TRUE;
	}

	if (!any) {
		TurnSetup *setup = latest_turn(self);

		if (setup != NULL)
			setup->tool_calls++;
	}
}

guint
clawt_agent_get_turn_tool_calls_in(
	ClawtAgent  *self,
	const gchar *room_id
){
	TurnSetup *setup;

	g_return_val_if_fail(CLAWT_IS_AGENT(self), G_MAXUINT);

	setup = turn_in(self, room_id);

	/*
	 * A room with no turn on record has no count, and "unknown" must
	 * not read as zero to a caller deciding whether the turn did
	 * anything.
	 */
	return setup != NULL ? setup->tool_calls : G_MAXUINT;
}

gboolean
clawt_agent_describe_turn_in(
	ClawtAgent   *self,
	const gchar  *room_id,
	const gchar **origin,
	const gchar **task_id,
	const gchar **model
){
	TurnSetup *setup;

	g_return_val_if_fail(CLAWT_IS_AGENT(self), FALSE);

	setup = turn_in(self, room_id);
	if (origin != NULL)
		*origin = (setup != NULL) ? setup->origin : NULL;
	if (task_id != NULL)
		*task_id = (setup != NULL) ? setup->task_id : NULL;
	if (model != NULL)
		*model = (setup != NULL && setup->input != NULL)
			? clawt_mailbox_item_get_model(setup->input) : NULL;

	return setup != NULL;
}

/* The original delivery remains available after typing falls. */
ClawtMailboxItem *
clawt_agent_get_turn_input_in(ClawtAgent *self, const gchar *room_id)
{
	TurnSetup *setup;

	g_return_val_if_fail(CLAWT_IS_AGENT(self), NULL);
	setup = turn_in(self, room_id);
	if (setup == NULL && room_id == NULL)
		setup = latest_turn(self);
	return setup != NULL ? setup->input : NULL;
}

void
clawt_agent_set_turn_replies(ClawtAgent *self, gboolean replies)
{
    g_return_if_fail(CLAWT_IS_AGENT(self));

    pending_turn(self)->replies = replies;
}

void
clawt_agent_set_turn_origin(ClawtAgent *self, const gchar *from)
{
    TurnSetup *setup;

    g_return_if_fail(CLAWT_IS_AGENT(self));

    setup = pending_turn(self);
    g_free(setup->origin);
    setup->origin = g_strdup(from);
}

const gchar *
clawt_agent_get_turn_origin(ClawtAgent *self)
{
    GHashTableIter iter;
    gpointer key;
    gpointer value;
    TurnSetup *setup;

    g_return_val_if_fail(CLAWT_IS_AGENT(self), NULL);

    /*
     * Any peer origin among the turns running, preferring the most
     * recent.
     *
     * This is read by clawtilla_message_user's guard, and the question
     * it is really asking is "did a peer start what I am doing" -- to
     * which, with two turns running and one of them a peer's, the answer
     * is yes.  The name may then be the wrong peer, which is why the
     * refusal is worth keeping short of certainty: refusing is right,
     * and the agent can reach anybody with clawtilla_message_agent.
     */
    setup = latest_turn(self);

    if (setup != NULL && setup->origin != NULL &&
        room_is_live(self, setup->room))
        return setup->origin;

    g_hash_table_iter_init(&iter, self->turns);

    while (g_hash_table_iter_next(&iter, &key, &value)) {
        TurnSetup *other = value;

        if (other->origin != NULL &&
            g_hash_table_contains(self->typing_rooms, key))
            return other->origin;
    }

    if (setup != NULL)
        return setup->origin;

    setup = g_queue_peek_head(self->pending);

    return (setup != NULL) ? setup->origin : NULL;
}

void
clawt_agent_set_turn_task_id(ClawtAgent *self, const gchar *task_id)
{
    TurnSetup *setup;

    g_return_if_fail(CLAWT_IS_AGENT(self));

    setup = pending_turn(self);
    g_free(setup->task_id);
    setup->task_id = g_strdup(task_id);
}

const gchar *
clawt_agent_get_turn_task_id(ClawtAgent *self)
{
    GHashTableIter iter;
    gpointer key;
    gpointer value;
    const gchar *found = NULL;
    guint live = 0;

    g_return_val_if_fail(CLAWT_IS_AGENT(self), NULL);

    /*
     * Only when the turns running agree, and otherwise nothing.
     *
     * This becomes the parent of whatever the agent delegates, and a
     * wrong parent is worse than no parent: it hangs work under somebody
     * else's job, where the depth limit measures from the wrong place
     * and clawt_task_manager_cancel() reaches into a tree it does not
     * belong to.  No parent is what the behaviour was before any of this
     * existed, so ambiguity costs nothing that was not already absent.
     */
    g_hash_table_iter_init(&iter, self->turns);

    while (g_hash_table_iter_next(&iter, &key, &value)) {
        TurnSetup *setup = value;

        if (!g_hash_table_contains(self->typing_rooms, key))
            continue;

        live++;

        if (setup->task_id == NULL)
            continue;

        if (found != NULL && g_strcmp0(found, setup->task_id) != 0)
            return NULL;

        found = setup->task_id;
    }

    if (live > 0)
        return found;

    {
        TurnSetup *setup = latest_turn(self);

        if (setup != NULL)
            return setup->task_id;

        setup = g_queue_peek_head(self->pending);

        return (setup != NULL) ? setup->task_id : NULL;
    }
}

const gchar *
clawt_agent_get_turn_task_id_in(ClawtAgent *self, const gchar *room_id)
{
    TurnSetup *setup;

    g_return_val_if_fail(CLAWT_IS_AGENT(self), NULL);

    setup = turn_in(self, room_id);

    return (setup != NULL) ? setup->task_id
                           : clawt_agent_get_turn_task_id(self);
}

const gchar *
clawt_agent_get_turn_origin_in(ClawtAgent *self, const gchar *room_id)
{
    TurnSetup *setup;

    g_return_val_if_fail(CLAWT_IS_AGENT(self), NULL);

    setup = turn_in(self, room_id);

    return (setup != NULL) ? setup->origin
                           : clawt_agent_get_turn_origin(self);
}

gboolean
clawt_agent_is_typing_in(ClawtAgent *self, const gchar *room_id)
{
    g_return_val_if_fail(CLAWT_IS_AGENT(self), FALSE);

    return room_is_live(self, room_id);
}

ClawtTypingEdge
clawt_agent_note_typing(ClawtAgent  *self,
                        const gchar *room_id,
                        gboolean     typing)
{
    const gchar *key = room_key(room_id);
    gboolean was_typing;
    gboolean room_was_live;
    ClawtTypingEdge edges = CLAWT_TYPING_EDGE_NONE;

    g_return_val_if_fail(CLAWT_IS_AGENT(self), CLAWT_TYPING_EDGE_NONE);

    was_typing = g_hash_table_size(self->typing_rooms) > 0;
    room_was_live = g_hash_table_contains(self->typing_rooms, key);

    if (typing) {
        /*
         * A room already typing is a keepalive, not a turn.  This is the
         * whole of the frame-versus-edge fix: libreclaw re-sends the
         * same TRUE every 25 seconds, and taking each one as a turn
         * start reset the depth, re-opened closed exchanges, switched
         * off clawtilla_message_user's guard and dropped the task a
         * delegation would be parented on.
         */
        if (!g_hash_table_contains(self->typing_rooms, key)) {
            gint64 *started = g_new(gint64, 1);

            /*
             * When, on the same edge that starts the turn: a keepalive
             * does not move it, so "running for four minutes" is the
             * turn's age and not the time since the last refresh.
             */
            *started = g_get_real_time();
            g_hash_table_insert(self->typing_rooms, g_strdup(key), started);
            clawt_agent_begin_turn(self, room_id);
        }
    } else {
        /*
         * The turn's description stays in the table.  libreclaw lowers
         * the indicator *before* it posts the answer, so the daemon
         * reads this after the falling edge; a table that forgot the
         * room here would have nothing to judge that answer by.
         */
        g_hash_table_remove(self->typing_rooms, key);

		/*
		 * The turn ended, so a restart has nothing of it to rerun.
		 * Only on a real falling edge: a FALSE for a room that was not
		 * typing describes no turn, and closing the room's last one
		 * again is harmless but says something untrue.
		 */
		if (room_was_live)
			close_turn_records(self, turn_in(self, room_id));
    }

    if (was_typing != (g_hash_table_size(self->typing_rooms) > 0))
        edges |= CLAWT_TYPING_EDGE_AGENT;

    /*
     * The room's own edge, which the agent's does not imply.
     *
     * An agent already typing in one room and starting a second produces
     * no agent edge -- correctly, since the per-agent turn state must
     * not reset -- and used to produce nothing at all.  So the second
     * room was never entered into the room watch and nothing recorded
     * who was holding it: `rooms.turn_timeout_seconds` could only ever
     * fire on whichever room an idle agent happened to enter first.
     */
    if (room_was_live != g_hash_table_contains(self->typing_rooms, key))
        edges |= CLAWT_TYPING_EDGE_ROOM;

    return edges;
}

guint
clawt_agent_get_typing_rooms(ClawtAgent *self)
{
    g_return_val_if_fail(CLAWT_IS_AGENT(self), 0);

    return g_hash_table_size(self->typing_rooms);
}

/* Oldest turn first, so a listing reads in the order the work began. */
static gint
compare_started(gconstpointer a, gconstpointer b, gpointer user_data)
{
    GHashTable *rooms = user_data;
    gint64 first = *(const gint64 *)g_hash_table_lookup(rooms, *(const gchar *const *)a);
    gint64 second = *(const gint64 *)g_hash_table_lookup(rooms, *(const gchar *const *)b);

    return (first > second) - (first < second);
}

GPtrArray *
clawt_agent_dup_running_rooms(ClawtAgent *self)
{
    GPtrArray *out;
    GHashTableIter iter;
    gpointer key;

    g_return_val_if_fail(CLAWT_IS_AGENT(self), NULL);

    out = g_ptr_array_new_with_free_func(g_free);
    g_hash_table_iter_init(&iter, self->typing_rooms);
    while (g_hash_table_iter_next(&iter, &key, NULL))
        g_ptr_array_add(out, g_strdup(key));
    g_ptr_array_sort_with_data(out, compare_started, self->typing_rooms);

    return out;
}

gint64
clawt_agent_get_turn_started_in(ClawtAgent *self, const gchar *room_id)
{
    const gint64 *started;

    g_return_val_if_fail(CLAWT_IS_AGENT(self), 0);

    started = g_hash_table_lookup(self->typing_rooms, room_key(room_id));
    return started != NULL ? *started : 0;
}

void
clawt_agent_close_running_turns(
	ClawtAgent *self
){
	GHashTableIter iter;
	gpointer key;

	g_return_if_fail(CLAWT_IS_AGENT(self));

	g_hash_table_iter_init(&iter, self->typing_rooms);
	while (g_hash_table_iter_next(&iter, &key, NULL))
		close_turn_records(self, g_hash_table_lookup(self->turns, key));
}

void
clawt_agent_clear_typing(ClawtAgent *self)
{
    g_return_if_fail(CLAWT_IS_AGENT(self));

    g_hash_table_remove_all(self->typing_rooms);
}

void
clawt_agent_begin_turn(ClawtAgent *self, const gchar *room_id)
{
    const gchar *key;
    TurnSetup *setup = NULL;
    guint i;

    g_return_if_fail(CLAWT_IS_AGENT(self));

    key = room_key(room_id);

    /*
     * The oldest delivery waiting for *this* room, or an untagged one.
     *
     * Per room, because an agent runs a turn per session and a session
     * is a room: a burst of [room A, room B] drained in arrival order
     * described each turn with the other room's message.  An untagged
     * entry is one whose caller never named a room, and any room may
     * take it -- nothing in production is in that position, since the
     * router chose the room in the first place.
     */
    for (i = 0; i < g_queue_get_length(self->pending); i++) {
        TurnSetup *candidate = g_queue_peek_nth(self->pending, i);

        /*
         * Not a delivery still waiting to hear whether it was steered:
         * libreclaw answers a steer before it queues the message, so a
         * turn starting now is somebody else's.
         */
        if (candidate->steering)
            continue;

        if (candidate->room == NULL ||
            g_strcmp0(room_key(candidate->room), key) == 0) {
            setup = candidate;
            g_queue_remove(self->pending, candidate);
            break;
        }
    }

    /*
     * Unless nothing else is waiting for this room.  An agent that ran
     * the message as a turn without ever answering the steer -- one
     * that went away and came back mid-answer -- is running *this*
     * message, and its description is the right one.
     */
    for (i = 0; setup == NULL && i < g_queue_get_length(self->pending); i++) {
        TurnSetup *candidate = g_queue_peek_nth(self->pending, i);

        if (candidate->steering &&
            g_strcmp0(room_key(candidate->room), key) == 0) {
            setup = candidate;
            setup->steering = FALSE;
            g_queue_remove(self->pending, candidate);
        }
    }

    /*
     * A turn nothing delivered into starts a fresh chain.
     *
     * The depth answers "how far had the message I am handling come",
     * which is true of a *turn* rather than of an agent -- so a turn
     * that began somewhere the daemon never sees (Matrix, a webhook,
     * local, cmacs) must not inherit whatever the last peer delivery
     * left, or an agent eventually cannot delegate at all.  Its origin
     * is nobody the daemon can name, and its closing text is the whole
     * point of it, so it answers normally.
     */
    if (setup == NULL) {
        setup = g_new0(TurnSetup, 1);
        setup->replies = TRUE;
    }

    g_free(setup->room);
    setup->room = g_strdup(key);

    g_hash_table_replace(self->turns, g_strdup(key), setup);
    trim_turn_history(self);

    g_free(self->last_room);
    self->last_room = g_strdup(key);

    /* Never zero once a turn has begun: zero means "no turn known". */
    self->turn_serial++;
}

guint64
clawt_agent_get_turn_serial(ClawtAgent *self)
{
    g_return_val_if_fail(CLAWT_IS_AGENT(self), 0);

    return self->turn_serial;
}

void
clawt_agent_set_config(ClawtAgent *self, ClawtAgentConfig *config)
{
    g_return_if_fail(CLAWT_IS_AGENT(self));
    g_return_if_fail(config != NULL);

    if (self->config == config)
        return;

    g_clear_pointer(&self->config, clawt_agent_config_unref);
    self->config = clawt_agent_config_ref(config);

    /*
     * The shadow decision belongs to the configuration, so it is retaken
     * whenever the configuration is replaced.
     *
     * clawt_agent_new() reads it once, and without this that first answer
     * outlives the config that produced it: an operator who corrects the
     * offending key gets a reload that reports success, an agent that
     * still refuses to start, and `agent show` printing the corrected
     * value beside the old refusal.  The only way out was restarting the
     * daemon, which costs every other agent its turn.
     *
     * Only a stopped or shadowed agent is touched.  One that is running
     * keeps running: a config change is documented as applying at the
     * agent's next start, and killing a turn in progress because a key it
     * has already read became invalid would be a worse bargain than
     * carrying on with the old one.
     */
    clawt_agent_revalidate(self);
}

/**
 * clawt_agent_revalidate:
 * @self: a #ClawtAgent
 *
 * Retakes the shadow decision from the configuration the agent already
 * holds, and recomputes its capabilities.
 *
 * clawt_agent_set_config() does this when the configuration object is
 * *replaced*, and returns early when handed the one it already has --
 * which is what `agent set` does, since it edits the very object the
 * agent is holding.  So correcting the key an agent was shadowed for
 * changed the config, changed nothing about the agent, and left
 * `agent list` still saying `shadow`, which reads as the setting having
 * been ignored.
 *
 * Only a stopped or shadowed agent is touched.  One that is running keeps
 * running: a config change applies at the agent's next start, and killing
 * a turn in progress because a key it has already read became invalid
 * would be the worse bargain.
 */
void
clawt_agent_revalidate(ClawtAgent *self)
{
    g_return_if_fail(CLAWT_IS_AGENT(self));

    if (self->config == NULL)
        return;

    /*
     * The computer belongs to the configuration too.
     *
     * Marked rather than dropped, and marked whatever the agent's state
     * is: a running agent keeps working with the machine it already
     * has -- a config change is documented as applying at the next
     * start -- and the flag is what makes that next start honour it.
     * Dropping the object here instead would take the computer away
     * from an agent mid-turn.
     */
    self->computer_stale = TRUE;

    if (self->state == CLAWT_AGENT_STATE_SHADOW &&
        !clawt_agent_config_is_shadow(self->config))
        set_state(self, CLAWT_AGENT_STATE_STOPPED, NULL);
    else if (self->state == CLAWT_AGENT_STATE_STOPPED &&
             clawt_agent_config_is_shadow(self->config))
        set_state(self, CLAWT_AGENT_STATE_SHADOW,
                  clawt_agent_config_get_shadow_reason(self->config));

    recompute_caps(self);
}

void
clawt_agent_set_runtime(ClawtAgent *self, ClawtAgentRuntime *runtime)
{
    g_return_if_fail(CLAWT_IS_AGENT(self));

    if (self->runtime != NULL) {
        g_signal_handlers_disconnect_by_func(self->runtime,
                                             G_CALLBACK(on_runtime_exited),
                                             self);
        g_signal_handlers_disconnect_by_func(self->runtime,
                                             G_CALLBACK(on_runtime_started),
                                             self);
    }

    g_clear_object(&self->runtime);

    if (runtime != NULL) {
        self->runtime = g_object_ref(runtime);
        g_signal_connect(runtime, "exited", G_CALLBACK(on_runtime_exited),
                         self);
        g_signal_connect(runtime, "started", G_CALLBACK(on_runtime_started),
                         self);
    }

    recompute_caps(self);
}

ClawtAgentRuntime *
clawt_agent_get_runtime(ClawtAgent *self)
{
    g_return_val_if_fail(CLAWT_IS_AGENT(self), NULL);

    return self->runtime;
}

void
clawt_agent_set_computer(ClawtAgent *self, ClawtComputer *computer)
{
    g_return_if_fail(CLAWT_IS_AGENT(self));

    g_clear_object(&self->computer);

    if (computer != NULL)
        self->computer = g_object_ref(computer);

    /* Whatever it was built from is what the agent has now. */
    self->computer_stale = FALSE;

    recompute_caps(self);
}

gboolean
clawt_agent_computer_is_stale(ClawtAgent *self)
{
    g_return_val_if_fail(CLAWT_IS_AGENT(self), FALSE);

    return self->computer_stale;
}

ClawtComputer *
clawt_agent_get_computer(ClawtAgent *self)
{
    g_return_val_if_fail(CLAWT_IS_AGENT(self), NULL);

    return self->computer;
}

void
clawt_agent_set_desktop(ClawtAgent *self, ClawtDesktop *desktop)
{
    g_return_if_fail(CLAWT_IS_AGENT(self));

    g_clear_object(&self->desktop);

    if (desktop != NULL)
        self->desktop = g_object_ref(desktop);
}

ClawtDesktop *
clawt_agent_get_desktop(ClawtAgent *self)
{
    g_return_val_if_fail(CLAWT_IS_AGENT(self), NULL);

    return self->desktop;
}

/*
 * Who else is on a shared machine, and where this agent's own workspace
 * is inside it. Two agents writing to /mnt/clawtilla/workspace would be
 * writing to each other's files, so each has its own directory there --
 * and an agent that is not told which one is its own will find the
 * others first. The other users are named so a process it did not start
 * or a file it did not write has an explanation that is not "something
 * is wrong with the machine".
 */
static void
describe_sharing(ClawtAgent *self, GString *out)
{
    const gchar *use = self->config != NULL
        ? clawt_agent_config_get_string(self->config, "computer.use") : NULL;
    ClawtConfig *fleet;
    GPtrArray *agents;
    g_autoptr(GString) others = NULL;
    g_autofree gchar *workspace = NULL;
    guint count = 0;
    guint i;

    if (use == NULL || *use == '\0')
        return;

    fleet = clawt_agent_config_get_config(self->config);
    agents = fleet != NULL ? clawt_config_get_agents(fleet) : NULL;
    others = g_string_new(NULL);
    for (i = 0; agents != NULL && i < agents->len; i++) {
        ClawtAgentConfig *config = g_ptr_array_index(agents, i);
        const gchar *id = clawt_agent_config_get_id(config);

        if (g_strcmp0(id, clawt_agent_get_id(self)) == 0 ||
            g_strcmp0(clawt_agent_config_get_string(config, "computer.use"), use) != 0)
            continue;
        g_string_append_printf(others, "%s%s", count == 0 ? "" : ", ", id);
        count++;
    }

    workspace = clawt_computer_shared_workspace_path(clawt_agent_get_id(self));
    g_string_append_printf(out,
        " This is the shared computer '%s'%s%s%s. Your workspace inside it is %s; "
        "the other agents' workspaces sit beside it under %s and are theirs, so "
        "leave them alone. Processes and files you did not make may belong to "
        "them. There is one screen: while another agent is typing or clicking "
        "on it, your own input is refused for a short while and looking still "
        "works.",
        use,
        count > 0 ? ", which you share with " : "",
        count > 0 ? others->str : "",
        count > 0 ? "" : " (nobody else is on it at the moment)",
        workspace, CLAWT_SHARED_WORKSPACES_ROOT);
}

const gchar *
clawt_agent_computer_text_for_operator(const gchar  *computer_type,
                                       const gchar  *detail,
                                       const gchar **out_caption)
{
    if (out_caption != NULL)
        *out_caption = NULL;

    /*
     * No computer: said to the operator.  Absent and "none" are the same
     * answer -- a daemon older than the field, or an agent whose detail
     * was never reported, has nothing better to offer.
     */
    if (computer_type == NULL || *computer_type == '\0' ||
        g_strcmp0(computer_type, "none") == 0)
        return "This agent has no computer, so it cannot run commands, "
               "read files or reach a filesystem. Choose one under "
               "Computer in its settings to give it somewhere to work.";

    if (detail == NULL || *detail == '\0')
        return NULL;

    if (out_caption != NULL)
        *out_caption = "What the agent itself is told about its computer, "
                       "word for word -- the text that decides whether it "
                       "goes looking for a tool it does not have.";

    return detail;
}

gchar *
clawt_agent_describe_computer(ClawtAgent *self)
{
    g_autoptr(GString) out = NULL;
    g_autofree gchar *computer_text = NULL;

    g_return_val_if_fail(CLAWT_IS_AGENT(self), NULL);

    if (self->computer == NULL && self->desktop == NULL)
        return g_strdup("You have no computer.");

    out = g_string_new(NULL);

    if (self->computer != NULL) {
        computer_text = clawt_computer_describe(self->computer);

        if (computer_text != NULL)
            g_string_append(out, computer_text);

        describe_sharing(self, out);
    }

    /*
     * The desktop is described here and not by the computer, because it
     * is an add-on: an agent can have one alongside whichever computer it
     * was given, and the computer has never heard of it.
     *
     * It has to be said at all. The tools arrive through the agent's
     * .mcp.json, so an MCP client lists them and the agent can see
     * screenshot and key_press -- and has no way to know whether they
     * point at its own VM or at the screen the user is sitting in front
     * of. Those call for completely different amounts of caution, and
     * guessing wrong in the confident direction is the bad one.
     */
    if (self->desktop != NULL) {
        g_autofree gchar *desktop_text =
            clawt_desktop_describe(self->desktop);

        if (desktop_text != NULL) {
            if (out->len > 0)
                g_string_append_c(out, ' ');

            g_string_append(out, desktop_text);
        }
    }

    return g_string_free(g_steal_pointer(&out), FALSE);
}

void
clawt_agent_set_link(ClawtAgent *self, ClawtLink *link_)
{
    g_return_if_fail(CLAWT_IS_AGENT(self));

    g_clear_object(&self->link);

    if (link_ != NULL) {
        self->link = g_object_ref(link_);

        if (self->state == CLAWT_AGENT_STATE_STARTING ||
            self->state == CLAWT_AGENT_STATE_DEGRADED)
            set_state(self, CLAWT_AGENT_STATE_RUNNING, NULL);

        return;
    }

    /*
     * A running agent that loses its link is degraded, not stopped: the
     * process is still there, but nothing can be delivered to it, and
     * reporting it as running would make messages appear to be going
     * somewhere.
     */
    if (self->state == CLAWT_AGENT_STATE_RUNNING)
        set_state(self, CLAWT_AGENT_STATE_DEGRADED,
                  "the agent's process is up but it is not connected");
}

ClawtLink *
clawt_agent_get_link(ClawtAgent *self)
{
    g_return_val_if_fail(CLAWT_IS_AGENT(self), NULL);

    return self->link;
}

gboolean
clawt_agent_start(ClawtAgent *self, GError **error)
{
    g_return_val_if_fail(CLAWT_IS_AGENT(self), FALSE);

    if (self->state == CLAWT_AGENT_STATE_SHADOW) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_AGENT_STATE,
                    "agent '%s' cannot run: %s",
                    clawt_agent_get_id(self),
                    self->status_detail != NULL ? self->status_detail
                                                : "its configuration was not "
                                                  "understood");
        return FALSE;
    }

    if (!clawt_agent_config_get_boolean(self->config, "enabled")) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_AGENT_STATE,
                    "agent '%s' is disabled", clawt_agent_get_id(self));
        return FALSE;
    }

    if (self->state == CLAWT_AGENT_STATE_RUNNING ||
        self->state == CLAWT_AGENT_STATE_STARTING)
        return TRUE;

    if (self->runtime == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_AGENT_STATE,
                    "agent '%s' has no runtime attached",
                    clawt_agent_get_id(self));
        return FALSE;
    }

    /*
     * Starting, not running.  The process being up is not the same as being
     * reachable: it becomes running when its link arrives.
     */
    set_state(self, CLAWT_AGENT_STATE_STARTING, NULL);

    if (!clawt_agent_runtime_start(self->runtime, error)) {
        set_state(self, CLAWT_AGENT_STATE_ERROR,
                  (error != NULL && *error != NULL) ? (*error)->message
                                                    : "it would not start");
        return FALSE;
    }

    return TRUE;
}

void
clawt_agent_stop(ClawtAgent *self)
{
    g_return_if_fail(CLAWT_IS_AGENT(self));

    if (self->state == CLAWT_AGENT_STATE_STOPPED ||
        self->state == CLAWT_AGENT_STATE_SHADOW)
        return;

    set_state(self, CLAWT_AGENT_STATE_STOPPING, NULL);

    if (self->link != NULL) {
        clawt_link_close(self->link, "the agent is being stopped");
        g_clear_object(&self->link);
    }

    if (self->runtime != NULL)
        clawt_agent_runtime_stop(self->runtime);
    else
        set_state(self, CLAWT_AGENT_STATE_STOPPED, NULL);

    /*
     * The mailbox is deliberately untouched.  Messages queued for a stopped
     * agent wait for it to come back -- that is the entire point of having
     * one.
     */
}

void
clawt_agent_mark_shadow(ClawtAgent *self, const gchar *reason)
{
    g_return_if_fail(CLAWT_IS_AGENT(self));

    set_state(self, CLAWT_AGENT_STATE_SHADOW, reason);
}

void
clawt_agent_set_error(ClawtAgent *self, const gchar *reason)
{
    g_return_if_fail(CLAWT_IS_AGENT(self));

    set_state(self, CLAWT_AGENT_STATE_ERROR, reason);
}

static void
clawt_agent_dispose(GObject *object)
{
    ClawtAgent *self = CLAWT_AGENT(object);

    if (self->runtime != NULL)
        g_signal_handlers_disconnect_by_func(self->runtime,
                                             G_CALLBACK(on_runtime_exited),
                                             self);

    g_clear_object(&self->runtime);
    g_clear_pointer(&self->pending, queue_of_turns_free);
    g_clear_pointer(&self->turns, g_hash_table_unref);
    g_clear_pointer(&self->channel_rooms, g_hash_table_unref);
    g_clear_pointer(&self->typing_rooms, g_hash_table_unref);
    g_clear_pointer(&self->last_room, g_free);
    g_clear_object(&self->computer);
    g_clear_object(&self->desktop);
    g_clear_object(&self->link);
    g_clear_object(&self->mailbox);
    g_clear_object(&self->memory);
    g_clear_pointer(&self->config, clawt_agent_config_unref);

    G_OBJECT_CLASS(clawt_agent_parent_class)->dispose(object);
}

static void
clawt_agent_finalize(GObject *object)
{
    ClawtAgent *self = CLAWT_AGENT(object);

    g_clear_pointer(&self->status_detail, g_free);

    /*
     * Not reached by anything until a test set an activity peer.
     *
     * clawt_agent_set_activity() has strdup'd it since it was written
     * and frees the previous one on replacement, so the live agent was
     * never wrong -- only the last value survived the agent.  Every
     * fixture in the suite passed NULL, so the string was never
     * allocated and the leak could not fire; the first test to name a
     * peer found it immediately.
     */
    g_clear_pointer(&self->activity_peer, g_free);
    g_clear_pointer(&self->failover, json_object_unref);

    G_OBJECT_CLASS(clawt_agent_parent_class)->finalize(object);
}

static void
clawt_agent_class_init(ClawtAgentClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->dispose = clawt_agent_dispose;
    object_class->finalize = clawt_agent_finalize;

    /**
     * ClawtAgent::state-changed:
     * @self: the agent
     * @state: the new state
     * @detail: (nullable): what happened
     */
    signals[SIGNAL_STATE_CHANGED] =
        g_signal_new("state-changed", CLAWT_TYPE_AGENT, G_SIGNAL_RUN_LAST, 0,
                     NULL, NULL, NULL, G_TYPE_NONE, 2,
                     G_TYPE_INT, G_TYPE_STRING);

    /**
     * ClawtAgent::error:
     * @self: the agent
     * @message: what went wrong
     */
    signals[SIGNAL_ERROR] =
        g_signal_new("error", CLAWT_TYPE_AGENT, G_SIGNAL_RUN_LAST, 0,
                     NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
clawt_agent_init(ClawtAgent *self)
{
    self->state = CLAWT_AGENT_STATE_STOPPED;
    self->caps = CLAWT_AGENT_CAPS_NONE;
    self->pending = g_queue_new();
    self->channel_rooms = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                g_free, g_free);
    self->turns = g_hash_table_new_full(g_str_hash, g_str_equal,
                                        g_free, turn_setup_free);
    self->typing_rooms = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, g_free);
}
