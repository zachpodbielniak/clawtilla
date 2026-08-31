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
    GHashTable     *typing_rooms;  /* room -> NULL: the ones running */
    gchar          *last_room;
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
} TurnSetup;


static void
turn_setup_free(gpointer data)
{
    TurnSetup *setup = data;

    g_free(setup->room);
    g_free(setup->origin);
    g_free(setup->task_id);
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

        if (type == CLAWT_COMPUTER_HOST)
            caps |= CLAWT_AGENT_CAPS_HOST_CONTROL;
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
     * And with it every typing frame it had raised.  A runtime that
     * exits mid-turn sends no FALSE, so the set would keep a room for
     * ever -- and the restarted agent's first frame would not read as a
     * turn start, leaving its first turn describing whatever the dead
     * one had been answering.
     */
    clawt_agent_clear_typing(self);

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

        if (next->origin == NULL)
            next->origin = g_strdup(oldest->origin);
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

gboolean
clawt_agent_note_typing(ClawtAgent  *self,
                        const gchar *room_id,
                        gboolean     typing)
{
    const gchar *key = room_key(room_id);
    gboolean was_typing;

    g_return_val_if_fail(CLAWT_IS_AGENT(self), FALSE);

    was_typing = g_hash_table_size(self->typing_rooms) > 0;

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
            g_hash_table_add(self->typing_rooms, g_strdup(key));
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
    }

    return was_typing != (g_hash_table_size(self->typing_rooms) > 0);
}

guint
clawt_agent_get_typing_rooms(ClawtAgent *self)
{
    g_return_val_if_fail(CLAWT_IS_AGENT(self), 0);

    return g_hash_table_size(self->typing_rooms);
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

        if (candidate->room == NULL ||
            g_strcmp0(room_key(candidate->room), key) == 0) {
            setup = candidate;
            g_queue_remove(self->pending, candidate);
            break;
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

    if (self->runtime != NULL)
        g_signal_handlers_disconnect_by_func(self->runtime,
                                             G_CALLBACK(on_runtime_exited),
                                             self);

    g_clear_object(&self->runtime);

    if (runtime != NULL) {
        self->runtime = g_object_ref(runtime);
        g_signal_connect(runtime, "exited", G_CALLBACK(on_runtime_exited),
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
    self->turns = g_hash_table_new_full(g_str_hash, g_str_equal,
                                        g_free, turn_setup_free);
    self->typing_rooms = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, NULL);
}
