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

gint
clawt_mailbox_router_send(ClawtMailboxRouter  *self,
                          ClawtMessage        *message,
                          GError             **error)
{
    GPtrArray *members;  /* unowned: the room keeps its member list */
    ClawtRoom *room;
    const gchar *destination;
    const gchar *sender;
    guint queued = 0;
    guint i;

    g_return_val_if_fail(CLAWT_IS_MAILBOX_ROUTER(self), -1);
    g_return_val_if_fail(message != NULL, -1);

    destination = clawt_message_get_room_id(message);
    sender = clawt_message_get_sender_id(message);

    if (destination == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "the message has no destination");
        return -1;
    }

    room = clawt_room_manager_get(self->rooms, destination);

    if (room == NULL) {
        /*
         * Not a room, so it must be an agent -- and a message to an agent
         * is a message in the direct room between the two, which is what
         * makes a reply land in the same conversation rather than starting
         * a parallel one.
         */
        if (clawt_agent_manager_get(self->agents, destination) == NULL) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                        "there is no agent or room called '%s'", destination);
            return -1;
        }

        room = clawt_room_manager_get_direct(self->rooms, sender,
                                             destination);
    }

    /*
     * Checked before anything is written.  A runaway fan-out has to be
     * stopped at the source: by delivery time the messages already exist,
     * and refusing then means cleaning up rather than preventing.
     */
    if (self->guard != NULL &&
        !clawt_loop_guard_check(self->guard, message, error))
        return -1;

    clawt_room_append(room, message, NULL);

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
        clawt_event_set_detail(event, "from", sender);
        clawt_event_set_detail(event, "to", destination);
        clawt_event_set_detail(event, "body", clawt_message_get_body(message));

        if (clawt_message_get_task_id(message) != NULL)
            clawt_event_set_detail(event, "task",
                                   clawt_message_get_task_id(message));

        clawt_event_bus_publish(self->bus, event);
    }

    members = clawt_room_get_members(room);

    for (i = 0; i < members->len; i++) {
        const gchar *member = g_ptr_array_index(members, i);
        ClawtAgent *agent;
        ClawtMailbox *mailbox;
        g_autoptr(ClawtMailboxItem) item = NULL;
        g_autofree gchar *item_id = NULL;
        g_autoptr(GError) local = NULL;

        /* A sender does not receive its own message. */
        if (g_strcmp0(member, sender) == 0)
            continue;

        if (!clawt_room_message_is_for(room, message, member))
            continue;

        agent = clawt_agent_manager_get(self->agents, member);
        if (agent == NULL)
            continue;

        mailbox = clawt_agent_get_mailbox(agent);
        if (mailbox == NULL)
            continue;

        item = clawt_mailbox_item_new(sender, member,
                                      clawt_message_get_body(message));
        clawt_mailbox_item_set_room(item, clawt_room_get_id(room));
        clawt_mailbox_item_set_task_id(item,
                                       clawt_message_get_task_id(message));
        clawt_mailbox_item_set_depth(item, clawt_message_get_depth(message));

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

gint
clawt_mailbox_router_send_to(ClawtMailboxRouter  *self,
                             const gchar         *from,
                             const gchar         *target,
                             const gchar         *body,
                             const gchar         *task_id,
                             gint                 depth,
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

    for (;;) {
        g_autoptr(ClawtMailboxItem) item = NULL;
        g_autoptr(GError) error = NULL;

        item = clawt_mailbox_lease(mailbox, 0);
        if (item == NULL)
            break;

        if (!clawt_link_deliver(link,
                                clawt_mailbox_item_get_room(item),
                                clawt_mailbox_item_get_from(item),
                                NULL,
                                clawt_mailbox_item_get_body(item),
                                clawt_mailbox_item_get_task_id(item),
                                &error)) {
            /*
             * Put back and the drain stops here.  Continuing past a failed
             * delivery would hand the agent its messages out of order once
             * the link came back, which is worse than a pause.
             */
            clawt_mailbox_nack(mailbox, clawt_mailbox_item_get_id(item),
                               error != NULL ? error->message
                                             : "delivery failed",
                               NULL);
            break;
        }

        /*
         * The agent is told how far this message has come, so anything it
         * sends in response counts as one hop further.  Without it every
         * outbound message looked like the first and max_hops could never
         * be reached.
         */
        clawt_agent_set_hop_depth(agent, clawt_mailbox_item_get_depth(item));

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
