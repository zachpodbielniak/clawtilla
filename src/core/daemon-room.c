/*
 * daemon-room.c - The client surface: msg.send and room.*
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"

#include <glib/gstdio.h>
#include <string.h>

#include "core/clawt-daemon.h"
#include "core/clawt-daemon-private.h"

/*
 * The room a request names, resolving an agent id to the direct room
 * with that agent.
 *
 * One resolver for every verb that takes a room, because the rule is
 * not obvious and a second copy of it would be a second answer.  A
 * client showing a conversation must not have to know how a direct room
 * is named -- the GTK client did not, and every chat opened empty with
 * a "no such room" behind it until this existed.
 *
 * Returns: (nullable) (transfer none): the room, or %NULL
 */
static ClawtRoom *
resolve_room(ClawtDaemon *self, const gchar *room_id, const gchar *viewer)
{
    ClawtRoom *room;

    if (room_id == NULL)
        return NULL;

    room = clawt_room_manager_get(self->rooms, room_id);

    if (room != NULL)
        return room;

    if (clawt_agent_manager_get(self->agents, room_id) == NULL)
        return NULL;

    return clawt_room_manager_get_direct(
        self->rooms, viewer != NULL ? viewer : "user", room_id);
}


/*
 * Rooms in the order a sidebar draws them.
 *
 * By `order` and then by id, so a fleet where nobody has arranged
 * anything is still stable rather than in hash-table order -- a list
 * that reshuffles on every refresh is a list nobody can click in.
 */
static gint
compare_rooms_by_order(gconstpointer a, gconstpointer b)
{
    ClawtRoom *left = *(ClawtRoom **)a;
    ClawtRoom *right = *(ClawtRoom **)b;
    gint left_order = clawt_room_get_order(left);
    gint right_order = clawt_room_get_order(right);

    if (left_order != right_order)
        return (left_order < right_order) ? -1 : 1;

    return g_strcmp0(clawt_room_get_id(left), clawt_room_get_id(right));
}

/*
 * Makes a room, everywhere a room has to exist.
 *
 * Three of them: the `rooms:` entry that lets it survive a restart, the
 * live object the router delivers through, and the event both clients
 * redraw on.  One function because the MCP tool reached the room
 * manager alone -- so a room an agent made was gone at the next restart
 * with its transcript orphaned, and every later edit to it reported
 * success while writing nothing, since clawt_config_set_room_*() has no
 * entry to write to.
 *
 * The config write comes first so a refusal there costs nothing, and is
 * undone if the manager then refuses -- the two validate the same
 * things, but a caller should not be left with half a room either way.
 *
 * Returns: (transfer none) (nullable): the room, or %NULL with @error
 */
ClawtRoom *
clawt_daemon_create_room(ClawtDaemon  *self,
                         const gchar  *room_id,
                         const gchar  *name,
                         const gchar  *members,
                         GError      **error)
{
    ClawtRoom *room;

    g_return_val_if_fail(CLAWT_IS_DAEMON(self), NULL);

    if (room_id == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "which room should be created?");
        return NULL;
    }

    if (!clawt_config_add_room(self->config, room_id, name, members, error))
        return NULL;

    room = clawt_room_manager_create(self->rooms, room_id, name, error);

    if (room == NULL) {
        clawt_config_remove_room(self->config, room_id);
        return NULL;
    }

    if (members != NULL) {
        g_auto(GStrv) parts = g_strsplit(members, ",", -1);
        gsize i;

        for (i = 0; parts[i] != NULL; i++) {
            const gchar *member = g_strstrip(parts[i]);

            if (*member != '\0')
                clawt_room_add_member(room, member);
        }
    }

    {
        g_autoptr(GError) save_error = NULL;

        if (!clawt_config_save(self->config, &save_error))
            g_warning("room %s was created but not saved: %s", room_id,
                      save_error->message);
    }

    clawt_event_bus_emit(self->bus, "room.created", room_id);

    return room;
}

/* ── Read marks ──────────────────────────────────────────────────── */

/* The file the marks live in, under the state directory. */
#define READS_DATABASE_NAME "reads.db"

void
clawt_daemon_reads_setup(ClawtDaemon *self)
{
    g_autofree gchar *path = NULL;
    g_autoptr(GError) error = NULL;

    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (self->state_dir == NULL || self->reads != NULL)
        return;

    path = g_build_filename(self->state_dir, READS_DATABASE_NAME, NULL);
    self->reads = clawt_read_store_new(path, &error);

    if (self->reads == NULL) {
        /*
         * A warning rather than a refusal to start.  Without the store
         * room.list simply leaves `unread` out, and every client falls
         * back to the count it keeps from live events -- the behaviour
         * of every build before this one, which is a smaller loss than
         * a fleet that will not come up.
         */
        g_warning("rooms: could not open %s, so unread counts are kept "
                  "by each client alone: %s", path,
                  error != NULL ? error->message : "unknown");
        return;
    }

    /*
     * A file made just now: every room is read up to its newest
     * message.  Without this the upgrade that introduces the store
     * would greet the operator with every message the fleet ever sent
     * them counted as unread -- months of conversations already read in
     * a client that kept the count in memory.
     */
    if (self->rooms != NULL && clawt_read_store_was_created(self->reads)) {
        g_autoptr(GPtrArray) rooms = clawt_room_manager_list(self->rooms);
        guint i;

        for (i = 0; i < rooms->len; i++) {
            ClawtRoom *room = g_ptr_array_index(rooms, i);
            const gchar *last_id = NULL;
            gint64 last_ts = 0;

            if (!clawt_room_get_latest(room, &last_id, &last_ts))
                continue;

            if (!clawt_read_store_set(self->reads, clawt_room_get_id(room),
                                      last_id, last_ts, &error)) {
                g_warning("rooms: could not seed the read mark of %s: %s",
                          clawt_room_get_id(room), error->message);
                g_clear_error(&error);
            }
        }
    }
}

void
clawt_daemon_reads_teardown(ClawtDaemon *self)
{
    g_return_if_fail(CLAWT_IS_DAEMON(self));

    g_clear_object(&self->reads);
}

guint
clawt_daemon_room_unread(ClawtDaemon *self, ClawtRoom *room)
{
    g_autofree gchar *last_id = NULL;
    gint64 last_ts = 0;

    g_return_val_if_fail(CLAWT_IS_DAEMON(self), 0);

    if (self->reads == NULL || room == NULL)
        return 0;

    clawt_read_store_get(self->reads, clawt_room_get_id(room), &last_id,
                         &last_ts);

    return clawt_room_count_unread(room, last_id, last_ts);
}

/*
 * The id a room's mark names, or "" for a room never read.
 *
 * "" rather than leaving the member out, because the two mean different
 * things to a client: no member is a daemon that holds no marks, and
 * clawt_unread_rule_index() draws no rule for it; "" is a room nobody
 * has read, where the rule goes above the first message.
 */
static gchar *
read_upto_of(ClawtDaemon *self, ClawtRoom *room)
{
    g_autofree gchar *last_id = NULL;

    clawt_read_store_get(self->reads, clawt_room_get_id(room), &last_id,
                         NULL);

    return last_id != NULL ? g_steal_pointer(&last_id) : g_strdup("");
}

/*
 * Moves one room's mark and says so.
 *
 * @upto_id names the last message read, or %NULL for the newest.  The
 * event goes out whether or not the mark moved, because a client that
 * asked expects its own badge -- and every other client's -- to clear,
 * and a second window may be holding a count from before the first one
 * read anything.
 *
 * Returns: %TRUE, or %FALSE with @error when @upto_id is not in the room
 *   or the write failed
 */
/*
 * Whether a mark at (@index, @ts) would move the room's held mark
 * backwards.
 *
 * Two clients read the same room and their requests arrive in either
 * order; the one that read less, landing second, must not undo the one
 * that read more -- or a conversation finished in one window lights up
 * again because a second window caught up late.  By position in the
 * history when the held message is still there, which is exact; by
 * timestamp when it is not.  Not by id: ids sort by time only to the
 * millisecond, and a burst inside one millisecond sorts at random.
 */
static gboolean
would_move_back(ClawtDaemon *self, ClawtRoom *room, gint index, gint64 ts)
{
    g_autofree gchar *held_id = NULL;
    gint64 held_ts = 0;
    gint held_index;

    if (!clawt_read_store_get(self->reads, clawt_room_get_id(room), &held_id,
                              &held_ts))
        return FALSE;

    held_index = clawt_room_message_index(room, held_id, NULL);

    if (held_index >= 0)
        return index < held_index;

    return ts < held_ts;
}

static gboolean
mark_room_read(ClawtDaemon  *self,
               ClawtRoom    *room,
               const gchar  *upto_id,
               GError      **error)
{
    g_autoptr(ClawtEvent) event = NULL;
    const gchar *last_id = NULL;
    gint64 last_ts = 0;
    gint index;

    if (upto_id != NULL) {
        /*
         * Only a message the room holds.  A mark naming nothing would
         * fall back to its timestamp, which a client could set to any
         * second it liked -- including one in the future, which would
         * then swallow every message until that second came round.
         */
        index = clawt_room_message_index(room, upto_id, &last_ts);

        if (index < 0) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                        "%s has no message %s", clawt_room_get_id(room),
                        upto_id);
            return FALSE;
        }

        last_id = upto_id;
    } else {
        index = clawt_room_get_latest(room, &last_id, &last_ts)
            ? (gint)clawt_room_get_message_count(room) - 1 : -1;
    }

    /*
     * An empty room has nothing to mark, and a mark with no message
     * behind it would only ever be a timestamp -- the case above that
     * can swallow messages.  Nothing is unread in it either way.  A mark
     * behind the one held is not written (see would_move_back()); the
     * event still goes out, so the asking client hears the room's real
     * count rather than nothing.
     */
    if (index >= 0 && !would_move_back(self, room, index, last_ts) &&
        !clawt_read_store_set(self->reads, clawt_room_get_id(room), last_id,
                              last_ts, error))
        return FALSE;

    {
        g_autofree gchar *held = read_upto_of(self, room);

        event = clawt_event_new("room.read", clawt_room_get_id(room));
        clawt_event_set_detail(event, "room", clawt_room_get_id(room));
        clawt_event_set_detail(event, "upto", held);
    }

    clawt_event_set_detail_int(event, "unread",
                               (gint64)clawt_daemon_room_unread(self, room));
    clawt_event_bus_publish(self->bus, event);

    return TRUE;
}

JsonNode *
clawt_daemon_handle_room(
    ClawtDaemon  *self,
    const gchar  *kind,
    JsonNode     *request,
    JsonObject   *payload,
    gboolean     *handled
)
{
    g_autoptr(JsonBuilder) builder = NULL;
    g_autoptr(GError) error = NULL;

    builder = json_builder_new();
    *handled = TRUE;

    /* ── messages and rooms ── */
    if (g_strcmp0(kind, "msg.send") == 0) {
        const gchar *target = clawt_ipc_payload_string(payload, "target");
        const gchar *body = clawt_ipc_payload_string(payload, "body");
        const gchar *from = clawt_ipc_payload_string(payload, "from");
        gboolean steering;
        gint queued;

        if (target == NULL || body == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "target and body are both required");

        /*
         * Under `agents.steer: all`, a person's message to an agent busy
         * in their conversation goes straight into that turn instead of
         * being held.  Said in the reply (`steering`), so a client can
         * tell the person; the transcript marks it once the agent says
         * the turn took it.
         */
        steering = g_strcmp0(from != NULL ? from : "user", "user") == 0 &&
                   clawt_daemon_turn_person_steers(self, target);

        /*
         * A correction typed at an agent that is already working is held
         * rather than routed, and does not enter the transcript yet.
         *
         * Appending it now would make the queued line the active leaf, so
         * the rest of the turn already in flight would hang off a line
         * the model was never shown -- and the transcript would read as
         * though the agent had answered something nobody had said.
         *
         * The reply says so, because a message that vanishes from the
         * composer and does not appear in the conversation reads exactly
         * like a message that was lost.
         */
        if (clawt_daemon_turn_steer(self, from != NULL ? from : "user",
                                    target, body)) {
            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "queued");
            json_builder_add_int_value(builder, 0);
            json_builder_set_member_name(builder, "steered");
            json_builder_add_boolean_value(builder, TRUE);
            json_builder_set_member_name(builder, "target_state");
            json_builder_add_string_value(builder, "running");
            json_builder_end_object(builder);

            return clawt_ipc_response_new(request,
                                          json_builder_get_root(builder));
        }

        queued = clawt_mailbox_router_send_to(self->router,
                                              from != NULL ? from : "user",
                                              target, body, NULL, 0, &error);

        if (queued < 0)
            return clawt_ipc_error_new(request, error->code, error->message);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "queued");
        json_builder_add_int_value(builder, queued);
        json_builder_set_member_name(builder, "steered");
        json_builder_add_boolean_value(builder, FALSE);
        json_builder_set_member_name(builder, "steering");
        json_builder_add_boolean_value(builder, steering);

        /*
         * Whether anything is going to read it.  A mailbox accepts a
         * message for a stopped agent by design -- that is the point of
         * making it durable -- but a client that cannot tell "queued" from
         * "delivered" leaves the user watching a spinner for an agent that
         * is not running and never will answer.  Reported only for a
         * single agent; for a room the members each have their own state
         * and the client can ask for them.
         */
        {
            ClawtAgent *agent = clawt_agent_manager_get(self->agents, target);

            if (agent != NULL) {
                json_builder_set_member_name(builder, "target_state");
                json_builder_add_string_value(
                    builder, clawt_enum_to_nick(CLAWT_TYPE_AGENT_STATE,
                                                clawt_agent_get_state(agent)));
            }
        }

        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "room.list") == 0) {
        g_autoptr(GPtrArray) rooms = clawt_room_manager_list(self->rooms);
        guint i;

        /*
         * Sorted here, not in each client.
         *
         * Grouping and ordering belong to whoever already decides them:
         * agent.list returns the fleet ordered, and a client that
         * sorted rooms itself would be a second answer to what order
         * the sidebar is in -- and the two would differ exactly once.
         */
        g_ptr_array_sort(rooms, compare_rooms_by_order);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "rooms");
        json_builder_begin_array(builder);

        for (i = 0; i < rooms->len; i++) {
            ClawtRoom *room = g_ptr_array_index(rooms, i);
            GPtrArray *members = clawt_room_get_members(room);
            guint j;

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "id");
            json_builder_add_string_value(builder, clawt_room_get_id(room));
            json_builder_set_member_name(builder, "name");
            json_builder_add_string_value(builder, clawt_room_get_name(room));
            json_builder_set_member_name(builder, "members");
            json_builder_begin_array(builder);

            for (j = 0; j < members->len; j++)
                json_builder_add_string_value(
                    builder, g_ptr_array_index(members, j));

            json_builder_end_array(builder);

            /*
             * Enough for a client to draw a conversation list without
             * fetching every transcript to find out which rooms have
             * anything in them.  A fleet accumulates a direct room per
             * pair, and most of them are empty.
             */
            json_builder_set_member_name(builder, "messages");
            json_builder_add_int_value(
                builder, clawt_room_get_message_count(room));

            /*
             * How many of them the operator has not read, and the mark
             * that count runs from -- held here rather than in each
             * client, so reading in one clears the count in all of them
             * and a restart forgets nothing.  Left out when there is no
             * read store, which every client takes as "keep counting
             * from events", the behaviour before marks existed.
             */
            if (self->reads != NULL) {
                g_autofree gchar *upto = read_upto_of(self, room);

                json_builder_set_member_name(builder, "unread");
                json_builder_add_int_value(
                    builder, clawt_daemon_room_unread(self, room));
                json_builder_set_member_name(builder, "read_upto");
                json_builder_add_string_value(builder, upto);
            }

            /*
             * What a sidebar needs to draw it beside the agents: where
             * it sits, whose group it is under, whether it is a room
             * somebody made rather than one the daemon derives, and
             * whether being in it means answering everything.
             */
            json_builder_set_member_name(builder, "group");
            json_builder_add_boolean_value(builder,
                                           clawt_room_is_group(room));
            json_builder_set_member_name(builder, "declared");
            json_builder_add_boolean_value(
                builder, clawt_room_is_declared(clawt_room_get_id(room)));
            /*
             * A "Problems to look at" room: derived, like a routine's,
             * but one the operator is meant to read, so the clients list
             * it under its team where a routine's room is left out.
             */
            json_builder_set_member_name(builder, "incidents");
            json_builder_add_boolean_value(
                builder, clawt_incident_room_is(clawt_room_get_id(room)));
            json_builder_set_member_name(builder, "require_mention");
            json_builder_add_boolean_value(
                builder, clawt_room_get_require_mention(room));
            json_builder_set_member_name(builder, "order");
            json_builder_add_int_value(builder, clawt_room_get_order(room));

            /*
             * The round the room is in and how long one thread of it may
             * run (rooms.max_chain), for the room's header and the
             * Orchestration settings page.
             */
            json_builder_set_member_name(builder, "round");
            json_builder_add_int_value(builder, clawt_room_get_round(room));
            json_builder_set_member_name(builder, "max_chain");
            json_builder_add_int_value(builder,
                                       clawt_room_get_max_chain(room));

            if (clawt_room_get_team(room) != NULL) {
                json_builder_set_member_name(builder, "team");
                json_builder_add_string_value(builder,
                                              clawt_room_get_team(room));
            }

            {
                g_autoptr(GPtrArray) last = clawt_room_get_history(room, 1);

                if (last->len > 0) {
                    ClawtMessage *message = g_ptr_array_index(last, 0);

                    json_builder_set_member_name(builder, "last_sender");
                    json_builder_add_string_value(
                        builder, clawt_message_get_sender_id(message));
                    json_builder_set_member_name(builder, "last_body");
                    json_builder_add_string_value(
                        builder, clawt_message_get_body(message));
                    json_builder_set_member_name(builder, "last_ts");
                    json_builder_add_int_value(
                        builder, clawt_message_get_timestamp(message));
                }
            }

            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    /*
     * What the turn running in a room has done so far.
     *
     * For a client that opened the room while an agent was already
     * working: both clients rebuild a chat pane on every room switch,
     * so without this a switch away and back throws the running turn's
     * history away and leaves a typing dot with no explanation.
     *
     * An empty list is the ordinary answer, not a failure -- a room
     * with no turn running has no steps, and so does one whose agent
     * has not reached a tool yet.  Answered as an empty array rather
     * than a refusal, because a handler that calls an ordinary
     * condition an error gets one toast per refresh stacked over the
     * controls underneath it.
     */
    if (g_strcmp0(kind, "room.steps") == 0) {
        const gchar *room_id = clawt_ipc_payload_string(payload, "room");
        const gchar *viewer = clawt_ipc_payload_string(payload, "as");
        ClawtRoom *room = resolve_room(self, room_id, viewer);
        g_autoptr(GPtrArray) steps = NULL;
        guint i;

        /*
         * The same room/as pair room.history takes, through the same
         * resolver, so a client that can draw a conversation can ask
         * what is happening in it without a second way of naming it.
         *
         * A room that does not resolve answers with nothing rather than
         * refusing.  This is polled while a turn runs, and a handler
         * that calls an ordinary condition an error stacks one toast
         * per refresh over the controls underneath -- which is exactly
         * what computer.frame did while a VM booted.
         */
        steps = (room != NULL)
            ? clawt_daemon_room_steps(self, clawt_room_get_id(room))
            : g_ptr_array_new();

        /*
         * `live` narrows the answer to the running turn: the steps
         * taken since the room's last message.
         *
         * The rule is here rather than in a client because a client
         * that draws steps in two places -- interleaved through the
         * transcript, and again as a live tail -- needs the two to
         * agree about where one ends and the other begins, or the
         * turn that just finished is drawn twice.  Deciding that from
         * the room's own last message is the only definition that
         * moves at the right moment: the answer arriving is exactly
         * what turns a running turn into history.
         */
        if (room != NULL &&
            clawt_ipc_payload_boolean(payload, "live", FALSE)) {
            g_autoptr(GPtrArray) recent = clawt_room_get_history(room, 1);
            ClawtMessage *last = (recent != NULL && recent->len > 0)
                ? g_ptr_array_index(recent, 0) : NULL;
            gint64 since = (last != NULL)
                ? clawt_message_get_timestamp(last) : 0;
            const gchar *since_id = (last != NULL)
                ? clawt_message_get_id(last) : NULL;
            g_autoptr(GPtrArray) live = g_ptr_array_new();
            guint k;

            /*
             * clawt_turn_step_precedes_message(), not a comparison
             * written out here, and with the message's id so a step from
             * the same second as it lands on the right side.  A step is
             * stamped in microseconds and a message in seconds, so
             * `step_ts > message_ts` is true of every step
             * ever taken -- the filter passed everything, and the web
             * client drew each turn twice: once interleaved through the
             * transcript and again as a live tail below it.
             *
             * The helper existed and said so in its own documentation
             * when this was written.  A rule applied at one call site
             * is a rule about that call site.
             */
            for (k = 0; k < steps->len; k++) {
                ClawtTurnStep *one = g_ptr_array_index(steps, k);

                if (!clawt_turn_step_precedes_message(one, since, since_id))
                    g_ptr_array_add(live, one);
            }

            g_clear_pointer(&steps, g_ptr_array_unref);
            steps = g_steal_pointer(&live);
        }

        json_builder_begin_object(builder);

        json_builder_set_member_name(builder, "room");
        json_builder_add_string_value(
            builder, room != NULL ? clawt_room_get_id(room) : "");

        json_builder_set_member_name(builder, "steps");
        json_builder_begin_array(builder);

        for (i = 0; steps != NULL && i < steps->len; i++)
            clawt_daemon_add_step_object(builder, g_ptr_array_index(steps, i));

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "room.create") == 0) {
        const gchar *room_id = clawt_ipc_payload_string(payload, "room");
        ClawtRoom *room;

        room = clawt_daemon_create_room(
            self, room_id,
            clawt_ipc_payload_string(payload, "name"),
            clawt_ipc_payload_string(payload, "members"), &error);

        if (room == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        /*
         * The mention rule, when the caller said anything about it.
         * Otherwise the room resolves it from its own shape, which is
         * the answer somebody editing the file by hand also gets.
         */
        if (json_object_has_member(payload, "require_mention")) {
            gboolean require =
                clawt_ipc_payload_boolean(payload, "require_mention", TRUE);

            clawt_room_set_require_mention(room, require);
            clawt_config_set_room_boolean(self->config, room_id,
                                          "require_mention", require);

            if (!clawt_config_save(self->config, &error))
                g_warning("room %s was created but its mention rule was "
                          "not saved: %s", room_id, error->message);
        }

        return clawt_ipc_response_new(request, NULL);
    }

    if (g_strcmp0(kind, "room.remove") == 0) {
        const gchar *room_id = clawt_ipc_payload_string(payload, "room");

        if (room_id == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "which room should be removed?");

        if (!clawt_room_is_declared(room_id))
            return clawt_ipc_error_new(
                request, CLAWT_ERROR_INVALID_ARGUMENT,
                "that conversation is not a room somebody made -- it "
                "follows from who exists, so there is no entry to remove "
                "and it would come back the moment they spoke again");

        if (clawt_room_manager_get(self->rooms, room_id) == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such room");

        clawt_config_remove_room(self->config, room_id);
        clawt_room_manager_remove(self->rooms, room_id);

        if (!clawt_config_save(self->config, &error))
            g_warning("room %s was removed but the config was not saved: "
                      "%s", room_id, error->message);

        clawt_event_bus_emit(self->bus, "room.changed", room_id);

        /*
         * The transcript stays.  Removing a room is a configuration
         * change; destroying the record of what was said in it is not,
         * and is not recoverable.  Reported, because nothing will
         * reopen that file on its own.
         */
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "transcript_kept");
        json_builder_add_boolean_value(builder, TRUE);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "room.set") == 0) {
        const gchar *room_id = clawt_ipc_payload_string(payload, "room");
        const gchar *members = clawt_ipc_payload_string(payload, "members");
        const gchar *name = clawt_ipc_payload_string(payload, "name");
        ClawtRoom *room;

        if (room_id == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "which room should be changed?");

        if (!clawt_room_is_declared(room_id))
            return clawt_ipc_error_new(
                request, CLAWT_ERROR_INVALID_ARGUMENT,
                "that conversation's members follow from who exists, so "
                "there is nothing here to edit");

        room = clawt_room_manager_get(self->rooms, room_id);

        if (room == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such room");

        if (name != NULL) {
            clawt_room_set_name(room, name);
            clawt_config_set_room_string(self->config, room_id, "name",
                                         name);
        }

        /*
         * Which team's group it appears under, which is presentation and
         * nothing else -- it changes neither who is in the room nor who
         * a message reaches.  An empty string takes it off a team, since
         * that and "no team key at all" have to be spellable apart.
         */
        if (json_object_has_member(payload, "team")) {
            const gchar *team = clawt_ipc_payload_string(payload, "team");
            gboolean none = (team == NULL || *team == '\0');

            clawt_room_set_team(room, none ? NULL : team);
            clawt_config_set_room_string(self->config, room_id, "team",
                                         none ? NULL : team);
        }

        if (members != NULL) {
            g_auto(GStrv) parts = g_strsplit(members, ",", -1);
            GPtrArray *current = clawt_room_get_members(room);
            g_autoptr(GPtrArray) stale =
                g_ptr_array_new_with_free_func(g_free);
            gsize i;
            guint k;

            for (i = 0; parts[i] != NULL; i++) {
                const gchar *member = g_strstrip(parts[i]);

                if (*member != '\0')
                    clawt_room_add_member(room, member);
            }

            /*
             * And the ones no longer named.  Collected before removing,
             * since removing during the walk frees the strings it is
             * reading.
             */
            for (k = 0; k < current->len; k++) {
                const gchar *member = g_ptr_array_index(current, k);
                gboolean listed = FALSE;

                for (i = 0; parts[i] != NULL; i++) {
                    if (g_strcmp0(g_strstrip(parts[i]), member) == 0) {
                        listed = TRUE;
                        break;
                    }
                }

                if (!listed)
                    g_ptr_array_add(stale, g_strdup(member));
            }

            for (k = 0; k < stale->len; k++)
                clawt_room_remove_member(room,
                                         g_ptr_array_index(stale, k));

            clawt_config_set_room_members(self->config, room_id, members);
        }

        if (json_object_has_member(payload, "require_mention")) {
            gboolean require =
                clawt_ipc_payload_boolean(payload, "require_mention", TRUE);

            clawt_room_set_require_mention(room, require);
            clawt_config_set_room_boolean(self->config, room_id,
                                          "require_mention", require);
        }

        /*
         * How long one thread of a round may run.  Refused when
         * negative rather than wrapped, and bounded so a typo cannot
         * turn the cap into "never": past 100 replies a thread is not a
         * conversation, and max_hops would have stopped it long before.
         */
        if (json_object_has_member(payload, "max_chain")) {
            gint64 chain = clawt_ipc_payload_int(payload, "max_chain", -1);

            if (chain < 0 || chain > 100)
                return clawt_ipc_error_new(
                    request, CLAWT_ERROR_INVALID_ARGUMENT,
                    "max_chain is how many replies one thread of a round "
                    "may run to: 0 (no cap) to 100");

            clawt_room_set_max_chain(room, (guint)chain);
            clawt_config_set_room_int(self->config, room_id, "max_chain",
                                      chain);
        }

        if (!clawt_config_save(self->config, &error))
            g_warning("room %s was changed but not saved: %s", room_id,
                      error->message);

        clawt_event_bus_emit(self->bus, "room.changed", room_id);

        return clawt_ipc_response_new(request, NULL);
    }

    if (g_strcmp0(kind, "room.add") == 0) {
        const gchar *room_id = clawt_ipc_payload_string(payload, "room");
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        ClawtRoom *room = clawt_room_manager_get(self->rooms, room_id);

        if (room == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such room");

        /*
         * Checked rather than passed straight through: without it a
         * request with no agent named added nobody and reported success,
         * which is the one answer a caller cannot act on.
         */
        if (agent_id == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "which agent should be added?");

        /*
         * And not into a conversation that is derived rather than
         * declared.
         *
         * This would happily add a third member to `dm:oryx:user`, and
         * room_for() honours membership as permission -- so that agent
         * could then read and post into the operator's private
         * conversation with another.  Only the operator can reach this
         * verb, so it was a footgun rather than an escalation; putting
         * room editing in a client is exactly what would make somebody
         * do it by accident.
         */
        if (!clawt_room_is_declared(room_id))
            return clawt_ipc_error_new(
                request, CLAWT_ERROR_INVALID_ARGUMENT,
                "that conversation's members follow from who exists -- "
                "adding somebody to it would give them a private "
                "conversation that is not theirs");

        clawt_room_add_member(room, agent_id);

        {
            g_autofree gchar *list = clawt_room_member_list(room);

            clawt_config_set_room_members(self->config, room_id, list);
        }

        if (!clawt_config_save(self->config, &error))
            g_warning("room %s gained %s but was not saved: %s", room_id,
                      agent_id, error->message);

        clawt_event_bus_emit(self->bus, "room.changed", room_id);

        return clawt_ipc_response_new(request, NULL);
    }

    /*
     * The operator has read a conversation -- or all of them.
     *
     * `{room}` or `{agent}` names one: an agent means the operator's
     * direct room with it, named by clawt_room_manager_direct_id() and
     * never spelled out here, because "dm:user:" + id is right for some
     * agents and wrong for every one whose id sorts before "user".
     * `upto` is the last message read, the newest by default.
     * `{all:true}` reads every room.  Each room marked publishes
     * `room.read`, which is what clears the badge in every client.
     */
    if (g_strcmp0(kind, "room.read") == 0) {
        const gchar *room_id = clawt_ipc_payload_string(payload, "room");
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        const gchar *upto = clawt_ipc_payload_string(payload, "upto");
        ClawtRoom *room = NULL;

        if (self->reads == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
                                       "read marks are not available: the "
                                       "read store could not be opened");

        if (clawt_ipc_payload_boolean(payload, "all", FALSE)) {
            g_autoptr(GPtrArray) rooms = clawt_room_manager_list(self->rooms);
            guint marked = 0;
            guint i;

            /*
             * Only the rooms with something unread are touched, so "mark
             * all as read" on a fleet with hundreds of quiet direct rooms
             * publishes one event per conversation that actually
             * changes, not one per room the daemon knows of.
             */
            for (i = 0; i < rooms->len; i++) {
                ClawtRoom *each = g_ptr_array_index(rooms, i);

                if (clawt_daemon_room_unread(self, each) == 0)
                    continue;

                if (!mark_room_read(self, each, NULL, &error))
                    return clawt_ipc_error_new(request, error->code,
                                               error->message);

                marked++;
            }

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "marked");
            json_builder_add_int_value(builder, marked);
            json_builder_end_object(builder);

            return clawt_ipc_response_new(request,
                                          json_builder_get_root(builder));
        }

        if (agent_id != NULL) {
            if (clawt_agent_manager_get(self->agents, agent_id) == NULL)
                return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                           "no such agent");

            /* Named by clawt_room_manager_direct_id(), inside. */
            room = clawt_room_manager_get_direct(self->rooms, "user",
                                                 agent_id);
        } else {
            room = resolve_room(self, room_id, NULL);
        }

        if (room == NULL)
            return clawt_ipc_error_new(request,
                                       room_id == NULL
                                           ? CLAWT_ERROR_INVALID_ARGUMENT
                                           : CLAWT_ERROR_NOT_FOUND,
                                       room_id == NULL
                                           ? "say which room was read, or "
                                             "all:true"
                                           : "no such room or agent");

        if (!mark_room_read(self, room, upto, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        {
            g_autofree gchar *read_upto = read_upto_of(self, room);

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "room");
            json_builder_add_string_value(builder, clawt_room_get_id(room));
            json_builder_set_member_name(builder, "read_upto");
            json_builder_add_string_value(builder, read_upto);
            json_builder_set_member_name(builder, "unread");
            json_builder_add_int_value(builder,
                                       clawt_daemon_room_unread(self, room));
            json_builder_end_object(builder);
        }

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "room.history") == 0) {
        const gchar *room_id = clawt_ipc_payload_string(payload, "room");
        const gchar *viewer = clawt_ipc_payload_string(payload, "as");
        ClawtRoom *room = resolve_room(self, room_id, viewer);
        g_autoptr(GPtrArray) history = NULL;
        guint i;

        if (room == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such room or agent");

        history = clawt_room_get_history(
            room, (guint)clawt_ipc_payload_int(payload, "limit", 50));

        json_builder_begin_object(builder);

        /*
         * Which room this actually is, because the request may have
         * named an agent and let the daemon resolve the direct room. A
         * client that shows a conversation has to be able to tell
         * whether an incoming message belongs in it, and comparing
         * against the agent it asked for is not the same question --
         * that is how a reply from an agent to one of its peers ended up
         * drawn in the user's own chat with it.
         */
        json_builder_set_member_name(builder, "room");
        json_builder_add_string_value(builder, clawt_room_get_id(room));

        /*
         * Where the "New messages" rule goes: above the first message
         * after this one that somebody else sent
         * (clawt_unread_rule_index()).  Read before the client marks
         * the room read on opening it, which is the order every client
         * asks in -- so the rule shows what was new when it was opened.
         */
        if (self->reads != NULL) {
            g_autofree gchar *upto = read_upto_of(self, room);

            json_builder_set_member_name(builder, "read_upto");
            json_builder_add_string_value(builder, upto);
        }

        json_builder_set_member_name(builder, "messages");
        json_builder_begin_array(builder);

        for (i = 0; i < history->len; i++) {
            ClawtMessage *message = g_ptr_array_index(history, i);

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "id");
            json_builder_add_string_value(builder,
                                          clawt_message_get_id(message));
            json_builder_set_member_name(builder, "sender");
            json_builder_add_string_value(
                builder, clawt_message_get_sender_id(message));
            json_builder_set_member_name(builder, "body");
            json_builder_add_string_value(builder,
                                          clawt_message_get_body(message));
            json_builder_set_member_name(builder, "ts");
            json_builder_add_int_value(
                builder, clawt_message_get_timestamp(message));

            /*
             * The task this message belongs to, when it belongs to one.
             * It is what turns a transcript into a chain you can follow:
             * without it a delegated reply is just another line from an
             * agent, with no sign of what asked for it.
             */
            if (clawt_message_get_task_id(message) != NULL) {
                json_builder_set_member_name(builder, "task");
                json_builder_add_string_value(
                    builder, clawt_message_get_task_id(message));
            }

            /*
             * How far this message had travelled agent-to-agent.  It is
             * what makes a runaway visible: a conversation whose hop
             * count climbs towards max_hops is a loop, and reading two
             * agents politely agreeing to do nothing gives no sign of
             * that at all.
             */
            json_builder_set_member_name(builder, "depth");
            json_builder_add_int_value(builder,
                                       clawt_message_get_depth(message));

			/*
			 * Present only when true, as in the transcript: an answer
			 * that woke nobody, and a note of results delivered as one
			 * turn.  Both integers, as the `message` event sends them
			 * (an event detail is a string or an int), so a client
			 * reads either source with the one integer reader.
			 */
			if (clawt_message_get_acknowledged(message)) {
				json_builder_set_member_name(builder, "acknowledged");
				json_builder_add_int_value(builder, 1);
			}

			if (clawt_message_get_merged(message) > 0) {
				json_builder_set_member_name(builder, "merged");
				json_builder_add_int_value(builder,
					(gint64)clawt_message_get_merged(message));
			}

			/* A consultation's question (1) or answer (2). */
			if (clawt_message_get_consult(message) != CLAWT_CONSULT_NONE) {
				json_builder_set_member_name(builder, "consult");
				json_builder_add_int_value(builder,
					(gint64)clawt_message_get_consult(message));
			}

			/* A turn cut short and not resumed; an integer, as above. */
			if (clawt_message_get_interrupted(message)) {
				json_builder_set_member_name(builder, "interrupted");
				json_builder_add_int_value(builder, 1);
			}

			/*
			 * Steered into the turn its recipient was running: the
			 * outcome, so a client draws it as steered and not as a
			 * message waiting for a turn of its own.
			 */
			if (clawt_message_get_steered(message) != NULL) {
				json_builder_set_member_name(builder, "steered");
				json_builder_add_string_value(builder,
					clawt_message_get_steered(message));
			}

			/*
			 * The round, and any mention withheld with its reason, as
			 * the `message` event carries them, so a client draws a
			 * history and a live message the same way.
			 */
			if (clawt_message_get_round(message) > 0) {
				json_builder_set_member_name(builder, "round");
				json_builder_add_int_value(builder,
					(gint64)clawt_message_get_round(message));
			}

			/*
			 * A cross-talk summary's jump targets: the Flow rooms and
			 * where in each the exchange began, space-separated and
			 * paired by position, as the `message` event carries them.
			 */
			if (clawt_message_get_crosstalk_rooms(message) != NULL) {
				json_builder_set_member_name(builder, "crosstalk_room");
				json_builder_add_string_value(builder,
					clawt_message_get_crosstalk_rooms(message));
				json_builder_set_member_name(builder, "crosstalk_message");
				json_builder_add_string_value(builder,
					clawt_message_get_crosstalk_messages(message));
			}

			if (clawt_message_get_answer_from(message) != NULL) {
				json_builder_set_member_name(builder, "answer_from");
				json_builder_add_string_value(builder,
					clawt_message_get_answer_from(message));
			}

			if (clawt_message_get_waiting_on(message) != NULL) {
				json_builder_set_member_name(builder, "waiting_on");
				json_builder_add_string_value(builder,
					clawt_message_get_waiting_on(message));
			}

			if (clawt_message_get_skipped(message) != NULL) {
				json_builder_set_member_name(builder, "skipped");
				json_builder_add_string_value(builder,
					clawt_message_get_skipped(message));
				json_builder_set_member_name(builder, "skip_reason");
				json_builder_add_string_value(builder,
					clawt_message_get_skip_reason(message) != NULL
						? clawt_message_get_skip_reason(message) : "");
			}

            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    *handled = FALSE;
    return NULL;
}
