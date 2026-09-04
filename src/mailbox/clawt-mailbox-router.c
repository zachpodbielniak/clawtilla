/*
 * clawt-mailbox-router.c - Getting a message to the right mailboxes
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "mailbox/clawt-mailbox-router.h"

struct _ClawtMailboxRouter {
    GObject parent_instance;

    ClawtAgentManager *agents;
    ClawtRoomManager  *rooms;
    ClawtLoopGuard    *guard;
    ClawtEventBus     *bus;      /* unowned */

    /* Where every routed message is indexed for recall.  Owned. */
    ClawtTranscriptIndex *transcripts;
};

G_DEFINE_FINAL_TYPE(ClawtMailboxRouter, clawt_mailbox_router, G_TYPE_OBJECT)

ClawtMailboxRouter *
clawt_mailbox_router_new(ClawtAgentManager *agents,
                         ClawtRoomManager  *rooms,
                         ClawtLoopGuard    *guard)
{
    ClawtMailboxRouter *self = g_object_new(CLAWT_TYPE_MAILBOX_ROUTER, NULL);

    g_return_val_if_fail(CLAWT_IS_AGENT_MANAGER(agents), NULL);
    g_return_val_if_fail(CLAWT_IS_ROOM_MANAGER(rooms), NULL);

    self->agents = g_object_ref(agents);
    self->rooms = g_object_ref(rooms);

    if (guard != NULL)
        self->guard = g_object_ref(guard);

    return self;
}

void
clawt_mailbox_router_set_event_bus(ClawtMailboxRouter *self,
                                   ClawtEventBus      *bus)
{
    g_return_if_fail(CLAWT_IS_MAILBOX_ROUTER(self));

    self->bus = bus;
}

void
clawt_mailbox_router_set_transcript_index(ClawtMailboxRouter *self,
                                          ClawtTranscriptIndex *index)
{
    g_return_if_fail(CLAWT_IS_MAILBOX_ROUTER(self));

    g_clear_object(&self->transcripts);

    if (index != NULL)
        self->transcripts = g_object_ref(index);
}

static void
publish(ClawtMailboxRouter *self, const gchar *kind, const gchar *subject,
        const gchar *from, const gchar *detail_key, const gchar *detail)
{
    g_autoptr(ClawtEvent) event = NULL;

    if (self->bus == NULL)
        return;

    event = clawt_event_new(kind, subject);
    clawt_event_set_detail(event, "from", from);

    if (detail_key != NULL)
        clawt_event_set_detail(event, detail_key, detail);

    clawt_event_bus_publish(self->bus, event);
}

/*
 * The room a message is bound for, resolved the one way.
 *
 * An agent id resolves to the direct room between sender and recipient,
 * which is what makes a reply land in the same conversation rather than
 * starting a parallel one.  send() and record() share this because two
 * resolvers would be two answers about where a message went.
 */
static ClawtRoom *
resolve_room(ClawtMailboxRouter  *self,
             ClawtMessage        *message,
             GError             **error)
{
    const gchar *destination = clawt_message_get_room_id(message);
    const gchar *sender = clawt_message_get_sender_id(message);
    ClawtRoom *room;

    if (destination == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "the message has no destination");
        return NULL;
    }

    room = clawt_room_manager_get(self->rooms, destination);

    if (room != NULL) {
        /*
         * Naming a room is not permission to post in it.
         *
         * room_for() learned this on the read side and the write side
         * kept resolving any id straight through: clawtilla_post_room
         * checked membership before calling, and clawtilla_message_agent
         * -- whose argument is an *agent* id -- did not check anything,
         * so "dm:oryx:user" reached here as a destination and was
         * accepted.  A message written by one agent then landed in the
         * operator's private conversation with another, was appended to
         * its transcript, published as a `message` event both clients
         * render in that chat, and queued into the other agent's mailbox
         * as though the operator had sent it.  The link path reaches the
         * same resolver with a room id out of the agent's own frame, so
         * the check belongs here rather than at either caller.
         *
         * Only a sender that is an agent is held to it.  The operator
         * and CLAWT_SYSTEM_SENDER are not in the manager, and a daemon
         * that cannot write into a room it is telling somebody about is
         * a daemon that cannot deliver a task notice.
         */
        if (clawt_agent_manager_get(self->agents, sender) != NULL &&
            !clawt_room_has_member(room, sender)) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED,
                        "'%s' is not in room '%s'", sender, destination);
            return NULL;
        }

        return room;
    }

    if (clawt_agent_manager_get(self->agents, destination) == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "there is no agent or room called '%s'", destination);
        return NULL;
    }

    return clawt_room_manager_get_direct(self->rooms, sender, destination);
}

/*
 * A message into the record: the room, the transcript index and the
 * event bus -- and nothing else.  This is the half of routing that says
 * what happened; send() adds the half that makes something happen.
 */
static void
record_in_room(ClawtMailboxRouter *self, ClawtRoom *room,
               ClawtMessage *message)
{
    clawt_room_append(room, message, NULL);

    /*
     * Indexed here, beside the append, because this is where the room is
     * known.  A failure is warned about and nothing else: a search that
     * has lost a line is worse than nothing to search, but a message
     * that was not delivered because it could not be indexed is worse
     * than both.
     */
    if (self->transcripts != NULL) {
        g_autoptr(GError) indexing = NULL;

        if (!clawt_transcript_index_add(self->transcripts,
                                        clawt_room_get_id(room), message,
                                        &indexing))
            g_warning("transcript index: %s", indexing->message);
    }

    /*
     * One event per message, published here because this is the only
     * place that knows which room it ended up in.
     *
     * It used to be published by the daemon's link handler instead,
     * carrying the sender and the body but not the room -- so a client
     * had nothing to match against and the GTK client fell back to "is
     * this from the agent I am looking at". A reply from that agent to
     * a *different* agent matched, and appeared in the user's own chat
     * with it. The message had gone to the right mailbox all along; the
     * transcript on screen was the thing that was wrong.
     */
    if (self->bus != NULL) {
        g_autoptr(ClawtEvent) event = NULL;

        event = clawt_event_new("message", clawt_room_get_id(room));
        clawt_event_set_detail(event, "id", clawt_message_get_id(message));
        clawt_event_set_detail(event, "from",
                               clawt_message_get_sender_id(message));
        clawt_event_set_detail(event, "to",
                               clawt_message_get_room_id(message));
        clawt_event_set_detail(event, "body",
                               clawt_message_get_body(message));

        if (clawt_message_get_task_id(message) != NULL)
            clawt_event_set_detail(event, "task",
                                   clawt_message_get_task_id(message));

        clawt_event_bus_publish(self->bus, event);
    }
}

gboolean
clawt_mailbox_router_record(ClawtMailboxRouter  *self,
                            ClawtMessage        *message,
                            GError             **error)
{
    ClawtRoom *room;

    g_return_val_if_fail(CLAWT_IS_MAILBOX_ROUTER(self), FALSE);
    g_return_val_if_fail(message != NULL, FALSE);

    room = resolve_room(self, message, error);

    if (room == NULL)
        return FALSE;

    record_in_room(self, room, message);

    return TRUE;
}


/*
 * Whether the closing text of the turn this delivery starts is a
 * message somebody will receive.
 *
 * Three answers, in order of how much they override.  The daemon's own
 * notices close an exchange whatever else is true.  A room that
 * requires mentions always posts the closing text -- it lands in the
 * transcript for everyone to read, and who it *reaches* is decided by
 * whether it names anybody, not by an invite bit; leaving the bit to
 * decide as well would silently swallow a reply that named somebody.
 * Otherwise it is the pair rule: a deliberate message earns one answer
 * and the answer earns none, and anything from outside the fleet is
 * always deliberate.
 */
static gboolean
turn_replies(gboolean system,
             gboolean peer,
             gboolean invites,
             gboolean room_requires_mention)
{
    if (system)
        return FALSE;

    if (room_requires_mention)
        return TRUE;

    return !peer || invites;
}

/*
 * What @agent_id has missed in @room since it last spoke there.
 *
 * A room that requires mentions delivers only the messages that name a
 * member, which is the whole point -- and it means an agent's context
 * holds its own corner of the conversation and nothing else.  The
 * transcript has what was said; the model does not.  A group chat whose
 * members each see a quarter of it is not a group chat.
 *
 * Bounded by `rooms.catchup_messages`, with a count of what was dropped
 * rather than a silent truncation: half a conversation presented as the
 * whole of it is worse than an honest gap, and an agent that knows it
 * missed something can call clawtilla_room_history.
 *
 * The agent's own last message is the watermark rather than a stored
 * one, because there is nothing to keep in step: an AI CLI cannot end a
 * turn without writing something, and in a room that requires mentions
 * that text is always posted -- so the last thing an agent said there
 * is the last time it was looking.
 *
 * Returns: (transfer full) (nullable): the block, or %NULL when there
 *   is nothing to catch up on
 */
static gchar *
room_catchup(ClawtRoom    *room,
             const gchar  *agent_id,
             const gchar  *from,
             const gchar  *body,
             guint         cap)
{
    g_autoptr(GPtrArray) history = NULL;
    g_autoptr(GPtrArray) missed = NULL;
    g_autoptr(GString) out = NULL;
    guint dropped = 0;
    guint i;

    if (cap == 0)
        return NULL;

    /*
     * One more than the cap, so the caller can tell "exactly the cap"
     * from "the cap and there was more".
     */
    history = clawt_room_get_history(room, 0);
    missed = g_ptr_array_new();

    for (i = history->len; i > 0; i--) {
        ClawtMessage *message = g_ptr_array_index(history, i - 1);

        /*
         * Not the message being delivered.  It is already in the
         * transcript by the time the drain runs -- record_in_room()
         * comes first -- and quoting it above itself reads as the room
         * having said everything twice.
         *
         * Matched on who said it and what they said, because the item
         * carries neither the message's id nor anything else that
         * would identify it: a mailbox item is minted per recipient
         * with an id of its own.
         */
        if (g_strcmp0(clawt_message_get_sender_id(message), from) == 0 &&
            g_strcmp0(clawt_message_get_body(message), body) == 0)
            continue;

        /* Back as far as the last thing this agent said, and no further. */
        if (g_strcmp0(clawt_message_get_sender_id(message), agent_id) == 0)
            break;

        if (missed->len >= cap) {
            dropped++;
            continue;
        }

        g_ptr_array_add(missed, message);
    }

    if (missed->len == 0)
        return NULL;

    out = g_string_new("[clawtilla] What has been said in this room since "
                       "you last did, oldest first");

    if (dropped > 0)
        g_string_append_printf(out, " (%u older message%s not shown -- "
                                    "clawtilla_room_history has them)",
                               dropped, dropped == 1 ? "" : "s");

    g_string_append(out, ":\n");

    /* Collected newest first, so read back the other way. */
    for (i = missed->len; i > 0; i--) {
        ClawtMessage *message = g_ptr_array_index(missed, i - 1);

        g_string_append_printf(out, "\n  %s: %s",
                               clawt_message_get_sender_id(message),
                               clawt_message_get_body(message));
    }

    return g_string_free(g_steal_pointer(&out), FALSE);
}

/*
 * How many members a delivery preamble names, and how much of each.
 *
 * These fields are operator-editable and, through an imported team,
 * third-party-authored -- and they are being interpolated into a
 * *trusted* prompt on every single delivery.  Capped and clipped for
 * the same reason clawt_mcp_tools_describe_for_agent()'s roster is.
 */
#define ROOM_ROSTER_MAX_MEMBERS (24)
#define ROOM_ROSTER_MAX_NAME    (60)

/*
 * The other members of @room, as the preamble lists them.
 *
 * Written per delivery rather than into a managed region of the agent's
 * workspace, because membership changes while an agent is running: a
 * region is refreshed at agent start, so an agent added to a group today
 * would learn about it after its next restart and a removed one would go
 * on being told it is still a member.  A preamble cannot go stale.
 *
 * Returns: (transfer full): the roster block, or %NULL when the agent is
 *   alone in the room
 */
static gchar *
room_roster(ClawtMailboxRouter *self,
            ClawtRoom          *room,
            const gchar        *recipient)
{
    GPtrArray *members = clawt_room_get_members(room);
    g_autoptr(GString) out = g_string_new(NULL);
    guint listed = 0;
    guint skipped = 0;
    guint i;

    for (i = 0; i < members->len; i++) {
        const gchar *member = g_ptr_array_index(members, i);
        ClawtAgent *agent;
        const gchar *name;

        if (g_strcmp0(member, recipient) == 0)
            continue;

        agent = clawt_agent_manager_get(self->agents, member);

        /*
         * Only real agents.  A daemon-owned room carries `routine` and
         * `trigger` as members so that its session keys come out right,
         * and telling an agent it can address one of those would be
         * offering it a name that reaches nothing.
         */
        if (agent == NULL)
            continue;

        if (listed >= ROOM_ROSTER_MAX_MEMBERS) {
            skipped++;
            continue;
        }

        name = clawt_agent_get_name(agent);

        if (name != NULL && g_strcmp0(name, member) != 0) {
            g_autofree gchar *clipped =
                clawt_clip_line(name, ROOM_ROSTER_MAX_NAME);

            g_string_append_printf(out, "  @%s (%s)\n", member, clipped);
        } else {
            g_string_append_printf(out, "  @%s\n", member);
        }

        listed++;
    }

    if (listed == 0)
        return NULL;

    /*
     * Said rather than silently dropped.  A roster that stops at
     * twenty-four without saying so reads as a complete list, and an
     * agent would conclude the twenty-fifth member is not in the room.
     */
    if (skipped > 0)
        g_string_append_printf(out, "  ...and %u more\n", skipped);

    return g_string_free(g_steal_pointer(&out), FALSE);
}

gint
clawt_mailbox_router_send(ClawtMailboxRouter  *self,
                          ClawtMessage        *message,
                          GError             **error)
{
    GPtrArray *members;  /* unowned: the room keeps its member list */
    g_autoptr(GPtrArray) recipients = NULL;  /* unowned ClawtAgent* */
    ClawtRoom *room;
    const gchar *sender;
    guint queued = 0;
    guint i;

    g_return_val_if_fail(CLAWT_IS_MAILBOX_ROUTER(self), -1);
    g_return_val_if_fail(message != NULL, -1);

    sender = clawt_message_get_sender_id(message);

    room = resolve_room(self, message, error);

    if (room == NULL)
        return -1;

    members = clawt_room_get_members(room);
    recipients = g_ptr_array_new();

    /*
     * Who this actually reaches, worked out before anything is spent on
     * it.
     *
     * In a room that requires mentions most messages reach nobody by
     * design -- a member thinking out loud, agreeing, noting something
     * -- and every one of those used to be charged against two limits
     * it has no business touching.  The rate limit is the smaller half.
     * The cycle detector is the sharp one: it fingerprints sender, room
     * and body, and for a peer sender an exact repeat inside
     * `orchestration.cycle_seconds` does not refuse but **stalls the
     * whole room**, which takes a person to undo and stalls every task
     * its members hold.  One agent writing "Acknowledged." twice in a
     * standup would have ended the standup.
     */
    for (i = 0; i < members->len; i++) {
        const gchar *member = g_ptr_array_index(members, i);
        ClawtAgent *agent;

        if (g_strcmp0(member, sender) == 0)
            continue;

        agent = clawt_agent_manager_get(self->agents, member);

        if (agent == NULL || clawt_agent_get_mailbox(agent) == NULL)
            continue;

        if (!clawt_room_message_is_for(room, message, member,
                                       clawt_agent_get_name(agent)))
            continue;

        g_ptr_array_add(recipients, agent);
    }

    /*
     * Checked before anything is written.  A runaway fan-out has to be
     * stopped at the source: by delivery time the messages already exist,
     * and refusing then means cleaning up rather than preventing.
     *
     * Working the recipients out first writes nothing, so the claim
     * above still holds -- what moved is the arithmetic, not the
     * record.
     *
     * Whether it reaches anybody is passed *in* rather than deciding
     * whether to ask.  Skipping the whole predicate for a costless
     * message also skipped the stall check, so a room that had been
     * ended went on accepting posts into its transcript, and the hop
     * check, so a message past max_hops that named nobody was recorded
     * instead of refused.
     */
    if (self->guard != NULL) {
        g_autoptr(GError) refusal = NULL;

        /*
         * With the destination room's own hop limit, which is why the
         * guard is consulted here rather than by the sender: this is the
         * first point that knows which room the message landed in, and
         * `rooms.max_hops` is a property of that room.
         *
         * It had been parsed onto the #ClawtRoom and read by nothing, so
         * a room declaring a limit was counted against the fleet's.  0
         * means the room said nothing and the fleet's applies.
         */
        if (!clawt_loop_guard_check_in_room(self->guard, message,
                                            clawt_room_get_max_hops(room),
                                            recipients->len > 0,
                                            &refusal)) {
            /*
             * Announced, not only returned.  A refusal on the link path
             * had nowhere to go but the log: the two agents simply
             * stopped, and whoever was watching saw a conversation trail
             * off with no indication that anything had stepped in.
             */
            if (self->bus != NULL) {
                g_autoptr(ClawtEvent) event = NULL;

                event = clawt_event_new("message.refused",
                                        clawt_room_get_id(room));
                clawt_event_set_detail(event, "from", sender);
                clawt_event_set_detail(event, "to",
                                       clawt_message_get_room_id(message));
                clawt_event_set_detail(event, "reason", refusal->message);
                clawt_event_bus_publish(self->bus, event);
            }

            g_propagate_error(error, g_steal_pointer(&refusal));
            return -1;
        }
    }

    /*
     * Recorded whoever it reaches.  A remark in a room that named
     * nobody is still what was said there, and the transcript is what a
     * member catching up reads.
     */
    record_in_room(self, room, message);

    for (i = 0; i < recipients->len; i++) {
        ClawtAgent *agent = g_ptr_array_index(recipients, i);
        const gchar *member = clawt_agent_get_id(agent);
        ClawtMailbox *mailbox = clawt_agent_get_mailbox(agent);
        g_autoptr(ClawtMailboxItem) item = NULL;
        g_autofree gchar *item_id = NULL;
        g_autoptr(GError) local = NULL;

        item = clawt_mailbox_item_new(sender, member,
                                      clawt_message_get_body(message));
        clawt_mailbox_item_set_room(item, clawt_room_get_id(room));
        clawt_mailbox_item_set_task_id(item,
                                       clawt_message_get_task_id(message));
        clawt_mailbox_item_set_depth(item, clawt_message_get_depth(message));

        /*
         * The band the sender asked for, carried onto every item the
         * message produces.
         *
         * Nothing outside a test had ever called
         * clawt_mailbox_item_set_priority(), so every item the fleet had
         * ever queued sat at the constructor's NORMAL -- while the
         * mailbox leased by band, `drop-oldest` shed the lowest band
         * first, and clawtilla_message_agent told agents that urgent
         * jumps the queue.  Four bands, one of them ever used.
         */
        clawt_mailbox_item_set_priority(item,
                                        clawt_message_get_priority(message));

        /*
         * And whether answering it is the recipient's job, which the
         * drain below turns into what the agent is told and what the
         * daemon does with the text it writes.
         */
        clawt_mailbox_item_set_invites_reply(
            item, clawt_message_get_invites_reply(message));

        item_id = clawt_mailbox_post(mailbox, item, &local);

        if (item_id == NULL) {
            /*
             * One full mailbox does not fail the whole post.  A room of
             * five where one member is backed up should still reach the
             * other four, and the failure is reported rather than hidden.
             */
            g_warning("could not queue for %s: %s", member,
                      local != NULL ? local->message : "unknown reason");
            continue;
        }

        queued++;

        publish(self, "mailbox.queued", member, sender, "room",
                clawt_room_get_id(room));
    }

    /*
     * Delivered immediately to whoever is connected, rather than waiting
     * for a periodic drain: a message that sits in a queue for a second
     * while its recipient is idle and connected reads as the fleet being
     * slow.
     */
    for (i = 0; i < members->len; i++)
        clawt_mailbox_router_drain(self, g_ptr_array_index(members, i));

    return (gint)queued;
}

gboolean
clawt_mailbox_router_note(ClawtMailboxRouter  *self,
                          const gchar         *target,
                          const gchar         *body,
                          GError             **error)
{
    g_autoptr(ClawtMessage) message = NULL;
    ClawtRoom *room;

    g_return_val_if_fail(CLAWT_IS_MAILBOX_ROUTER(self), FALSE);
    g_return_val_if_fail(target != NULL, FALSE);
    g_return_val_if_fail(body != NULL, FALSE);

    room = clawt_room_manager_get(self->rooms, target);

    if (room == NULL) {
        if (clawt_agent_manager_get(self->agents, target) == NULL) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                        "there is no agent or room called '%s'", target);
            return FALSE;
        }

        room = clawt_room_manager_get_direct(self->rooms, "user", target);
    }

    message = clawt_message_new(clawt_room_get_id(room),
                                CLAWT_SYSTEM_SENDER, body);

    /*
     * Recorded, not sent: no mailbox post and no drain, because a note
     * is for the people reading and the agent has just been interrupted
     * or cut off.  Through record_in_room() so a note is also indexed --
     * it was not, so "why did this stop" could never be recalled.
     */
    record_in_room(self, room, message);

    return TRUE;
}

gint
clawt_mailbox_router_send_to(ClawtMailboxRouter  *self,
                             const gchar         *from,
                             const gchar         *target,
                             const gchar         *body,
                             const gchar         *task_id,
                             gint                 depth,
                             GError             **error)
{
    return clawt_mailbox_router_send_to_full(self, from, target, body,
                                             task_id, depth,
                                             CLAWT_PRIORITY_NORMAL, error);
}

gint
clawt_mailbox_router_send_to_full(ClawtMailboxRouter  *self,
                                  const gchar         *from,
                                  const gchar         *target,
                                  const gchar         *body,
                                  const gchar         *task_id,
                                  gint                 depth,
                                  ClawtPriority        priority,
                                  GError             **error)
{
    g_autoptr(ClawtMessage) message = NULL;

    g_return_val_if_fail(CLAWT_IS_MAILBOX_ROUTER(self), -1);
    g_return_val_if_fail(from != NULL, -1);
    g_return_val_if_fail(target != NULL, -1);
    g_return_val_if_fail(body != NULL, -1);

    message = clawt_message_new(target, from, body);
    clawt_message_set_task_id(message, task_id);
    clawt_message_set_depth(message, depth);
    clawt_message_set_priority(message, priority);

    return clawt_mailbox_router_send(self, message, error);
}

guint
clawt_mailbox_router_drain(ClawtMailboxRouter *self, const gchar *agent_id)
{
    ClawtAgent *agent;
    ClawtMailbox *mailbox;
    ClawtLink *link;
    guint delivered = 0;

    g_return_val_if_fail(CLAWT_IS_MAILBOX_ROUTER(self), 0);

    if (agent_id == NULL)
        return 0;

    agent = clawt_agent_manager_get(self->agents, agent_id);
    if (agent == NULL)
        return 0;

    link = clawt_agent_get_link(agent);
    mailbox = clawt_agent_get_mailbox(agent);

    if (link == NULL || mailbox == NULL || !clawt_link_is_open(link))
        return 0;

    /*
     * An agent whose account is out of session allowance is not fed.
     *
     * Delivery acknowledges an item the moment it reaches the socket,
     * so draining into an agent that cannot answer *consumes* the queue:
     * every message is handed over, refused by the CLI without reaching
     * a model, and gone.  The limit is per account, so the whole fleet
     * is behind the same wall at the same moment and the entire backlog
     * can be spent in a couple of minutes -- which is exactly what
     * happened, and why two agents ended the afternoon holding nothing.
     *
     * Left queued instead.  The mailbox is durable precisely so work can
     * outlive an agent that cannot take it yet, and the next drain after
     * the reset delivers it in order.
     */
    {
        ClawtAgentRuntime *runtime = clawt_agent_get_runtime(agent);

        /*
         * Two reasons, asked separately because they are two reasons.
         * An account out of session allowance clears at a time we know;
         * an operator's hold clears when they say so, and must not be
         * lifted by the first.  The effect here is the same and the
         * effect elsewhere is not -- the restart accounting reads only
         * the first, and calling a held agent's death a rate limit
         * would send whoever reads that log to the wrong layer.
         */
        if (runtime != NULL &&
            (clawt_agent_runtime_is_held(runtime) ||
             clawt_agent_runtime_is_paused(
                 runtime, g_get_real_time() / G_USEC_PER_SEC)))
            return 0;
    }

    for (;;) {
        g_autoptr(ClawtMailboxItem) item = NULL;
        g_autoptr(GError) error = NULL;
        g_autofree gchar *body = NULL;
        ClawtRoom *item_room;
        gboolean room_requires_mention;
        const gchar *from;
        gboolean peer;
        gboolean system;
        gboolean invites;

        item = clawt_mailbox_lease(mailbox, 0);
        if (item == NULL)
            break;

        /*
         * The room this arrived in, looked up once.  Three things below
         * want it: which preamble the member is handed, whether its
         * closing text is a message at all, and the session hint.
         */
        item_room = clawt_room_manager_get(self->rooms,
                                           clawt_mailbox_item_get_room(item));
        room_requires_mention =
            item_room != NULL &&
            clawt_room_get_require_mention(item_room);

        /*
         * A message from a peer says so, in the body.
         *
         * The sender travels in its own field, and models do not
         * reliably notice it: an agent messaged by another agent read it
         * as coming from its operator, answered it as an instruction,
         * and the two of them talked past each other for fifty turns.
         * Saying it in the text is the difference between a peer asking
         * a question and the human giving an order.
         *
         * Applied at delivery, not stored: the transcript keeps what was
         * actually said.
         */
        from = clawt_mailbox_item_get_from(item);
        peer = from != NULL &&
               clawt_agent_manager_get(self->agents, from) != NULL;
        system = g_strcmp0(from, CLAWT_SYSTEM_SENDER) == 0;
        invites = clawt_mailbox_item_get_invites_reply(item);

        /*
         * And what will happen to what it writes back.
         *
         * Two texts because there are two situations, and the difference
         * between them is the whole mechanism: a message somebody chose
         * to send earns one answer, and that answer earns none.  The
         * agent is told which of the two it is holding, because the
         * previous single text asked it to "end your turn without
         * replying" if it had nothing to say -- and an AI CLI cannot do
         * that.  Whatever it writes at the end of a turn is the reply.
         * So the advice was unfollowable, both agents kept answering,
         * and a greeting ran until max_hops stopped it eight turns later.
         *
         * The instruction is now true either way: in the first case the
         * reply is delivered and is the last word, in the second it goes
         * nowhere and clawtilla_message_agent is named as the way to
         * reach them anyway.
         */
        /*
         * A group is a third situation, and it is not chosen by
         * `invites_reply`.
         *
         * The two texts below are both written for a pair -- "what you
         * write is sent back to '%s'" -- and in a room the reply goes to
         * the room.  Worse, every agent reply carries invites_reply
         * FALSE, so a member reached by another member's reply would be
         * handed the *closed exchange* text and told its answer goes
         * nowhere, in the one place where staying silent is the wrong
         * default.
         *
         * So the shape of the room decides, and the text says the four
         * things an agent cannot work out for itself: which room this
         * is, who else is in it, that `@name` is how to reach one of
         * them, and -- the part that decides whether any of this works
         * -- that naming nobody is the ordinary case.  An AI CLI cannot
         * end a turn without writing something, so a preamble that read
         * as "name somebody" would produce an agent that names somebody
         * every time, which is the runaway the mention rule exists to
         * prevent rebuilt out of prompt text.
         */
        if (room_requires_mention) {
            g_autofree gchar *roster =
                room_roster(self, item_room, agent_id);
            /*
             * And what it missed while nobody was naming it, which is
             * everything else said in the room.  Without this an agent
             * holds its own corner of the conversation and nothing
             * else: the transcript has what was said and the model does
             * not.
             */
            g_autofree gchar *catchup = room_catchup(
                item_room, agent_id, from,
                clawt_mailbox_item_get_body(item),
                clawt_room_get_catchup_messages(item_room));

            body = g_strdup_printf(
                "[clawtilla] The following was posted in room '%s', a "
                "conversation with several members -- not a message from "
                "your operator.\n"
                "\nWho else is here:\n%s"
                "\nWhat you write at the end of this turn is posted to "
                "this room. Everyone in it can read it, in their own "
                "time, and it costs nobody anything.\n"
                "\n**Most replies here should name nobody.** Think out "
                "loud, answer what was asked, agree, note something, say "
                "you are done -- all of that belongs in the room and "
                "reaches everyone who reads it. That is the normal way "
                "to take part.\n"
                "\nWriting @name puts the message in that member's "
                "queue and costs them a whole model turn, so do it only "
                "when you need *that member* to act now. Naming nobody "
                "is not rudeness and it is not silence: it is the room "
                "working as intended.\n"
                "\n@all reaches every member and belongs to your "
                "operator; if you write it, it reaches nobody. Name "
                "members individually instead.\n"
                "%s\n%s",
                clawt_mailbox_item_get_room(item),
                roster != NULL ? roster
                               : "  nobody else is in it yet\n",
                catchup != NULL ? catchup : "",
                clawt_mailbox_item_get_body(item));
        } else if (peer && invites)
            body = g_strdup_printf(
                "[clawtilla] The following is from '%s', another agent in "
                "your fleet -- not from your operator. Treat it as a "
                "colleague's message.\n"
                "What you write at the end of this turn is sent back to "
                "'%s' and ends the exchange -- they will read it and will "
                "not answer. So put everything you have to say into it, "
                "and do not expect another round. If you need something "
                "back from them, ask for it here.\n\n%s",
                from, from, clawt_mailbox_item_get_body(item));
        else if (peer)
            body = g_strdup_printf(
                "[clawtilla] The following is '%s' answering you, which "
                "closes the exchange.\n"
                "What you write at the end of this turn will NOT be sent "
                "to anybody -- read this, act on it, and end your turn. Do "
                "not write a reply or an acknowledgement; there is nowhere "
                "for one to go. If something genuinely has to reach '%s' "
                "-- an answer to a question they asked, or a correction "
                "that changes what they will do -- call "
                "clawtilla_message_agent, which starts a fresh exchange "
                "they may answer once.\n\n%s",
                from, from, clawt_mailbox_item_get_body(item));

        /*
         * And which conversation it is, named so the agent can say so.
         *
         * An agent can be mid-turn in several rooms at once, and a tool
         * call arrives at the daemon on a per-agent link with no room on
         * it.  libreclaw puts the session's room in the CLI's
         * environment, which is the answer when it is there; this is the
         * agent's own copy, for the paths where it is not, and for the
         * ordinary case of an agent needing to say which conversation it
         * is talking about.
         *
         * Appended rather than prepended: the message is what the agent
         * is being asked to act on, and a routing detail above it is
         * read as part of the request.
         */
        /*
         * A group delivery needs this whoever sent it.
         *
         * The condition used to be `peer`, which is right for a pair:
         * only another agent's message can arrive while the operator's
         * own conversation is also mid-turn.  A group is reached by the
         * operator too, and being in one is precisely the case where an
         * agent has several conversations at once -- so a message from
         * the operator into a standup is exactly when the hint is
         * needed and exactly when it was missing.
         */
        if ((peer || room_requires_mention) && body != NULL) {
            g_autofree gchar *addressed = g_steal_pointer(&body);

            body = g_strdup_printf(
                "%s\n\n[clawtilla] This conversation is room '%s'. If you "
                "call a clawtilla tool while handling this, pass "
                "turn_room: \"%s\" so it is answered for this "
                "conversation and not another one you are also in. It is "
                "not where the tool acts -- clawtilla_post_room still "
                "takes its own room_id -- it is which of your "
                "conversations you are in.",
                addressed, clawt_mailbox_item_get_room(item),
                clawt_mailbox_item_get_room(item));
        }

        /*
         * Which conversation the delivery belongs to, derived from the
         * room rather than stored: a delivery into a direct room is
         * part of that room's conversation, whoever wrote it.  For a
         * peer or the operator the peer *is* the sender and the hint
         * changes nothing (the link omits it).  It exists for the
         * senders that are nobody's counterparty -- "clawtilla" above
         * all: libreclaw keys sessions on (room, sender), so a settle
         * notice keyed on the system's own name matched no session and
         * allocated a fresh context per (room, system) pair, which
         * then acted on the notice knowing nothing else.  Computed
         * here, at the one place that builds the frame, so the next
         * system-sender message kind is covered without knowing it
         * needed to be.  A group room has no single counterparty and
         * gets no hint.
         */
        {
            const gchar *session_peer = NULL;

            if (item_room != NULL) {
                GPtrArray *members = clawt_room_get_members(item_room);
                g_autofree GStrv ids = NULL;
                guint mi;

                ids = g_new0(gchar *, members->len + 1);

                for (mi = 0; mi < members->len; mi++)
                    ids[mi] = (gchar *)g_ptr_array_index(members, mi);

                session_peer = clawt_chat_conversation_peer(
                    (const gchar *const *)ids, agent_id);
            }

            if (!clawt_link_deliver(link,
                                    clawt_mailbox_item_get_room(item),
                                    from,
                                    NULL,
                                    (body != NULL)
                                        ? body
                                        : clawt_mailbox_item_get_body(item),
                                    clawt_mailbox_item_get_task_id(item),
                                    session_peer,
                                    &error)) {
                /*
                 * Put back and the drain stops here.  Continuing past a
                 * failed delivery would hand the agent its messages out
                 * of order once the link came back, which is worse than
                 * a pause.
                 */
                clawt_mailbox_nack(mailbox,
                                   clawt_mailbox_item_get_id(item),
                                   error != NULL ? error->message
                                                 : "delivery failed",
                                   NULL);
                break;
            }
        }

        /*
         * One delivery, one entry, one turn.
         *
         * The depth, so anything the agent sends in response counts as a
         * hop further -- without it every outbound message looked like
         * the first and max_hops could never be reached.  Whether the
         * text it ends the turn with is a message: only a peer can close
         * an exchange, so an operator, a routine or anything else
         * outside the fleet always gets an answer.  And who is asking,
         * so the answer travels back up the chain it came down rather
         * than over their head into the operator's chat, which is what
         * clawtilla_message_user() would do.
         *
         * All three in one call, because they describe one message and
         * the agent runs one turn per message: LcSession queues them and
         * drain_next_message() pops a single entry per turn.  Set as
         * three separate fields on the agent, a burst of five collapsed
         * into one description and four turns ran with none.
         *
         * The reply flag is per item rather than accumulated across the
         * drain, for the same reason.  It was accumulated, on the
         * reasoning that the agent answers the whole drain in one turn
         * -- so an acknowledgement queued behind a question must not
         * decide for both.  It does not; each gets its own turn, and
         * accumulating made the acknowledgement's turn reply, which is
         * the loop the flag exists to end.
         *
         * And the task, which is what anything the agent delegates from
         * this turn is parented on.  Per item for the same reason as the
         * rest: a drain of [task delivery, ordinary message] must not
         * hang the ordinary message's turn off the task.
         *
         * And the room, so the turn that takes this entry is the turn in
         * that room.  An agent runs a turn per session and a session is
         * a room, so it can have several going at once; without the room
         * a burst across two of them was drained in arrival order and
         * each turn was described by the other room's message.
         */
        /*
         * The system's own messages are the third case, beside "a peer
         * chose to send this" and "a peer is answering": a notice.  The
         * system never wants an answer and there is nowhere for one to
         * go, so the turn it starts is a closed exchange whoever wrote
         * more recently -- and the notice's own text says so, because a
         * rule the agent cannot see is a rule it will violate.  The
         * origin stays "clawtilla", which is not an agent, so
         * clawtilla_message_user's back-up-the-chain guard does not
         * fire: relaying a settled task's result to the operator is
         * precisely what a notice's turn is for.
         */
        clawt_agent_deliver_turn(agent, clawt_mailbox_item_get_room(item),
                                 clawt_mailbox_item_get_depth(item),
                                 turn_replies(system, peer, invites,
                                              room_requires_mention),
                                 from,
                                 clawt_mailbox_item_get_task_id(item));

        /*
         * And who this turn is for.  Delivery is the only moment that
         * knows it: by the time the agent raises its typing indicator
         * the message is inside libreclaw and the sender is not
         * something the daemon can see any more.
         */
        clawt_agent_set_activity(agent, TRUE, from);

        /*
         * Acknowledged on the daemon's behalf as soon as it reaches the
         * socket.  The lease exists to survive a crash mid-turn; holding
         * it until the agent replies would redeliver every message the
         * agent chose not to answer.
         */
        clawt_mailbox_ack(mailbox, clawt_mailbox_item_get_id(item), NULL);
        delivered++;

        publish(self, "mailbox.delivered", agent_id,
                clawt_mailbox_item_get_from(item), NULL, NULL);
    }

    return delivered;
}

guint
clawt_mailbox_router_drain_all(ClawtMailboxRouter *self)
{
    GPtrArray *agents;
    guint delivered = 0;
    guint i;

    g_return_val_if_fail(CLAWT_IS_MAILBOX_ROUTER(self), 0);

    agents = clawt_agent_manager_list(self->agents);

    for (i = 0; i < agents->len; i++) {
        ClawtAgent *agent = g_ptr_array_index(agents, i);

        delivered += clawt_mailbox_router_drain(self,
                                                clawt_agent_get_id(agent));
    }

    return delivered;
}

guint
clawt_mailbox_router_sweep(ClawtMailboxRouter *self)
{
    GPtrArray *agents;
    guint affected = 0;
    guint i;

    g_return_val_if_fail(CLAWT_IS_MAILBOX_ROUTER(self), 0);

    agents = clawt_agent_manager_list(self->agents);

    for (i = 0; i < agents->len; i++) {
        ClawtAgent *agent = g_ptr_array_index(agents, i);
        ClawtMailbox *mailbox = clawt_agent_get_mailbox(agent);

        if (mailbox == NULL)
            continue;

        affected += clawt_mailbox_purge_expired(mailbox);
        affected += clawt_mailbox_reclaim_expired_leases(mailbox);
    }

    return affected;
}

static void
clawt_mailbox_router_dispose(GObject *object)
{
    ClawtMailboxRouter *self = CLAWT_MAILBOX_ROUTER(object);

    g_clear_object(&self->agents);
    g_clear_object(&self->rooms);
    g_clear_object(&self->guard);
    g_clear_object(&self->transcripts);

    G_OBJECT_CLASS(clawt_mailbox_router_parent_class)->dispose(object);
}

static void
clawt_mailbox_router_class_init(ClawtMailboxRouterClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = clawt_mailbox_router_dispose;
}

static void
clawt_mailbox_router_init(ClawtMailboxRouter *self)
{
    (void)self;
}
