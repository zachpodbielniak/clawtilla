/*
 * clawt-room-manager.c - The fleet's rooms and their transcripts
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "chat/clawt-room-manager.h"

#include <string.h>

struct _ClawtRoomManager {
    GObject parent_instance;

    GHashTable *rooms;          /* gchar* -> ClawtRoom* */
    GPtrArray  *order;          /* room ids, creation order */
    gchar      *transcript_dir;

    /*
     * Borrowed, and only so that a room cannot be named after an agent.
     * The daemon owns both and outlives this; a reference would be a
     * cycle, since the agent manager is reachable from the router which
     * is reachable from here.
     */
    ClawtAgentManager *agents;
};

G_DEFINE_FINAL_TYPE(ClawtRoomManager, clawt_room_manager, G_TYPE_OBJECT)

enum {
    SIGNAL_ROOM_ADDED,
    SIGNAL_ROOM_REMOVED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

ClawtRoomManager *
clawt_room_manager_new(const gchar *transcript_dir)
{
    ClawtRoomManager *self = g_object_new(CLAWT_TYPE_ROOM_MANAGER, NULL);

    if (transcript_dir != NULL)
        self->transcript_dir = clawt_expand_path(transcript_dir);

    return self;
}

static gchar *
transcript_path(ClawtRoomManager *self, const gchar *room_id)
{
    g_autofree gchar *filename = NULL;

    if (self->transcript_dir == NULL)
        return NULL;

    /*
     * The room id goes into a filename, so anything that could climb out
     * of the directory is replaced rather than rejected.  Ids are already
     * validated on creation; this is the second line, for ids that arrived
     * from a config file written by hand.
     */
    filename = g_strdup_printf("%s.ndjson", room_id);
    g_strdelimit(filename, "/\\", '_');

    return g_build_filename(self->transcript_dir, filename, NULL);
}

static void
load_room(ClawtRoomManager *self, ClawtRoom *room)
{
    g_autofree gchar *path = NULL;
    g_autofree gchar *contents = NULL;
    g_auto(GStrv) lines = NULL;
    gsize i;

    path = transcript_path(self, clawt_room_get_id(room));
    if (path == NULL || !g_file_get_contents(path, &contents, NULL, NULL))
        return;

    lines = g_strsplit(contents, "\n", -1);

    for (i = 0; lines[i] != NULL; i++) {
        g_autoptr(JsonParser) parser = NULL;
        g_autoptr(ClawtMessage) message = NULL;
        JsonObject *object;
        JsonNode *root;

        if (lines[i][0] == '\0')
            continue;

        parser = json_parser_new();

        /*
         * One unreadable line does not lose the transcript.  The file is
         * rewritten whole, so a crash mid-write can leave a partial tail;
         * dropping that line keeps everything before it.
         */
        if (!json_parser_load_from_data(parser, lines[i], -1, NULL))
            continue;

        root = json_parser_get_root(parser);
        if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root))
            continue;

        object = json_node_get_object(root);

        if (!json_object_has_member(object, "sender") ||
            !json_object_has_member(object, "body"))
            continue;

        message = clawt_message_new(
            clawt_room_get_id(room),
            json_object_get_string_member(object, "sender"),
            json_object_get_string_member(object, "body"));

        if (json_object_has_member(object, "id"))
            clawt_message_set_id(message,
                                 json_object_get_string_member(object, "id"));

        if (json_object_has_member(object, "sender_name"))
            clawt_message_set_sender_name(
                message, json_object_get_string_member(object,
                                                       "sender_name"));

        /*
         * Either spelling.  This writer has always used `ts`; the
         * append path used to write `timestamp`, and a file produced by
         * it would otherwise load with every message stamped now.
         */
        if (json_object_has_member(object, "ts"))
            clawt_message_set_timestamp(
                message, json_object_get_int_member(object, "ts"));
        else if (json_object_has_member(object, "timestamp"))
            clawt_message_set_timestamp(
                message, json_object_get_int_member(object, "timestamp"));

        if (json_object_has_member(object, "depth"))
            clawt_message_set_depth(
                message, (gint)json_object_get_int_member(object, "depth"));

        if (json_object_has_member(object, "task_id"))
            clawt_message_set_task_id(
                message, json_object_get_string_member(object, "task_id"));

        if (json_object_has_member(object, "parent_id"))
            clawt_message_set_parent_id(
                message, json_object_get_string_member(object, "parent_id"));

        /*
         * Restored, not appended: the transcript is append-only now, so
         * appending here would write every line back into the file it
         * was just read from.
         */
        clawt_room_restore(room, message);
    }
}

static ClawtRoom *
insert_room(ClawtRoomManager *self, ClawtRoom *room)
{
    const gchar *room_id = clawt_room_get_id(room);

    g_hash_table_replace(self->rooms, g_strdup(room_id), room);
    g_ptr_array_add(self->order, g_strdup(room_id));

    /*
     * The one place a room learns where its transcript lives, so every
     * room gets a file named for its id and no construction site can
     * pass something else.  Set before load_room(), which reads through
     * the same transcript_path() and so cannot disagree with it.
     */
    {
        g_autofree gchar *path = transcript_path(self, room_id);

        clawt_room_set_transcript_path(room, path);
    }

    load_room(self, room);

    g_signal_emit(self, signals[SIGNAL_ROOM_ADDED], 0, room);

    return room;
}

void
clawt_room_manager_set_agents(ClawtRoomManager  *self,
                              ClawtAgentManager *agents)
{
    g_return_if_fail(CLAWT_IS_ROOM_MANAGER(self));

    self->agents = agents;
}

ClawtRoom *
clawt_room_manager_create(ClawtRoomManager  *self,
                          const gchar       *room_id,
                          const gchar       *name,
                          GError           **error)
{
    ClawtRoom *room;

    g_return_val_if_fail(CLAWT_IS_ROOM_MANAGER(self), NULL);
    g_return_val_if_fail(room_id != NULL, NULL);

    if (!clawt_is_valid_id(room_id)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "'%s' is not a usable room id: use letters, digits, "
                    "'-' and '_'", room_id);
        return NULL;
    }

    if (g_hash_table_contains(self->rooms, room_id)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_ALREADY_EXISTS,
                    "there is already a room called '%s'", room_id);
        return NULL;
    }

    /*
     * And not a name the routing already means something by.
     *
     * `user`, `clawtilla`, `routine` and `trigger` are the senders every
     * routing rule keys on, and nothing checked them here at all -- a
     * room called `clawtilla` was creatable, and messages in it would
     * have been read as the daemon's own.
     */
    if (clawt_agent_id_is_reserved(room_id)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "'%s' is a sender name clawtilla's own routing keys "
                    "on, so it cannot also be a room", room_id);
        return NULL;
    }

    /*
     * Nor a name an agent already has.
     *
     * Every resolver in the tree tries a room first and falls back to
     * treating the id as an agent -- which is what lets a client ask for
     * a conversation by naming the agent.  A room called `oryx` would
     * therefore shadow the direct conversation with `oryx`, and the
     * symptom is a chat that opens on the wrong transcript rather than
     * anything that looks like a naming collision.
     */
    if (self->agents != NULL &&
        clawt_agent_manager_get(self->agents, room_id) != NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_ALREADY_EXISTS,
                    "'%s' is an agent, and a room of the same name would "
                    "hide your conversation with it -- pick another name",
                    room_id);
        return NULL;
    }

    /*
     * NULL, not `name`.  clawt_room_new()'s second argument is a
     * *transcript path*, and passing the room's display name here made
     * ClawtRoom try to write its own transcript to a file named after
     * whatever somebody had called the room.  Nothing had one configured,
     * so it never produced a stray file -- but the manager owns transcript
     * writing for its rooms, and handing the room a second, differently
     * shaped writer pointed at a made-up path is how it would have got
     * two.
     */
    room = clawt_room_new(room_id, NULL);

    /*
     * And it is actually called what it was called.
     *
     * @name was taken, documented, and then discarded -- every room
     * created through here, from the IPC verb and from
     * clawtilla_create_room alike, was displayed by its id.  A
     * parameter read by nobody looks exactly like one that works.
     */
    if (name != NULL && *name != '\0')
        clawt_room_set_name(room, name);

    return insert_room(self, room);
}

ClawtRoom *
clawt_room_manager_get(ClawtRoomManager *self, const gchar *room_id)
{
    g_return_val_if_fail(CLAWT_IS_ROOM_MANAGER(self), NULL);

    if (room_id == NULL)
        return NULL;

    return g_hash_table_lookup(self->rooms, room_id);
}

gchar *
clawt_room_manager_direct_id(const gchar *a, const gchar *b)
{
    g_return_val_if_fail(a != NULL, NULL);
    g_return_val_if_fail(b != NULL, NULL);

    /*
     * Sorted, so "a to b" and "b to a" name the same room, and joined
     * with a character an agent id cannot contain.
     *
     * Ids may contain '_', so joining with it made "a" + "b_c" and
     * "a_b" + "c" produce the same room -- two unrelated pairs sharing
     * one private conversation, neither of them actually a member of the
     * room they were handed.
     */
    if (g_strcmp0(a, b) <= 0)
        return g_strdup_printf("dm:%s:%s", a, b);

    return g_strdup_printf("dm:%s:%s", b, a);
}

/*
 * A room the daemon owns, named `<kind>:<id>`.
 *
 * One function for routines and triggers because they want exactly the
 * same thing and a second copy would drift -- and the way it would drift
 * is a trigger and a routine of the same name sharing a room, which
 * reads as one of them answering the other's work.
 *
 * The sender is the kind, not `user`: it is the other half of
 * libreclaw's session key, so a distinct room reached from the
 * operator's own sender would still land the run in their session.
 */
static ClawtRoom *
get_owned_room(ClawtRoomManager *self,
               const gchar      *kind,
               const gchar      *owner_id,
               const gchar      *agent_id)
{
    g_autofree gchar *room_id = NULL;
    ClawtRoom *room;

    room_id = g_strdup_printf("%s:%s", kind, owner_id);
    room = g_hash_table_lookup(self->rooms, room_id);

    if (room != NULL)
        return room;

    room = clawt_room_new(room_id, NULL);

    clawt_room_add_member(room, kind);
    clawt_room_add_member(room, agent_id);

    return insert_room(self, room);
}

ClawtRoom *
clawt_room_manager_get_routine(ClawtRoomManager *self,
                               const gchar      *routine_id,
                               const gchar      *agent_id)
{
    g_return_val_if_fail(CLAWT_IS_ROOM_MANAGER(self), NULL);
    g_return_val_if_fail(routine_id != NULL, NULL);
    g_return_val_if_fail(agent_id != NULL, NULL);

    return get_owned_room(self, "routine", routine_id, agent_id);
}

ClawtRoom *
clawt_room_manager_get_trigger(ClawtRoomManager *self,
                               const gchar      *trigger_id,
                               const gchar      *agent_id)
{
    g_return_val_if_fail(CLAWT_IS_ROOM_MANAGER(self), NULL);
    g_return_val_if_fail(trigger_id != NULL, NULL);
    g_return_val_if_fail(agent_id != NULL, NULL);

    return get_owned_room(self, "trigger", trigger_id, agent_id);
}

ClawtRoom *
clawt_room_manager_get_direct(ClawtRoomManager *self, const gchar *a,
                              const gchar *b)
{
    g_autofree gchar *room_id = NULL;
    ClawtRoom *room;

    g_return_val_if_fail(CLAWT_IS_ROOM_MANAGER(self), NULL);
    g_return_val_if_fail(a != NULL, NULL);
    g_return_val_if_fail(b != NULL, NULL);

    room_id = clawt_room_manager_direct_id(a, b);

    room = g_hash_table_lookup(self->rooms, room_id);
    if (room != NULL)
        return room;

    room = clawt_room_new(room_id, NULL);
    clawt_room_add_member(room, a);
    clawt_room_add_member(room, b);

    return insert_room(self, room);
}

guint
clawt_room_manager_load_direct(ClawtRoomManager *self)
{
    g_autoptr(GDir) dir = NULL;
    const gchar *name;
    guint restored = 0;

    g_return_val_if_fail(CLAWT_IS_ROOM_MANAGER(self), 0);

    if (self->transcript_dir == NULL)
        return 0;

    dir = g_dir_open(self->transcript_dir, 0, NULL);

    if (dir == NULL)
        return 0;

    while ((name = g_dir_read_name(dir)) != NULL) {
        g_autofree gchar *room_id = NULL;
        g_auto(GStrv) parts = NULL;

        if (!g_str_has_prefix(name, "dm:") ||
            !g_str_has_suffix(name, ".ndjson"))
            continue;

        room_id = g_strndup(name, strlen(name) - strlen(".ndjson"));

        if (g_hash_table_contains(self->rooms, room_id))
            continue;

        /*
         * The members come back out of the id rather than out of the
         * file: a transcript records who said what, not who is in the
         * room, and a room with no members delivers to nobody.
         */
        parts = g_strsplit(room_id, ":", 3);

        if (g_strv_length(parts) != 3 ||
            parts[1][0] == '\0' || parts[2][0] == '\0')
            continue;

        clawt_room_manager_get_direct(self, parts[1], parts[2]);
        restored++;
    }

    return restored;
}

GPtrArray *
clawt_room_manager_list(ClawtRoomManager *self)
{
    GPtrArray *out;
    guint i;

    g_return_val_if_fail(CLAWT_IS_ROOM_MANAGER(self), NULL);

    out = g_ptr_array_new();

    for (i = 0; i < self->order->len; i++) {
        ClawtRoom *room = g_hash_table_lookup(
            self->rooms, g_ptr_array_index(self->order, i));

        if (room != NULL)
            g_ptr_array_add(out, room);
    }

    return out;
}

gboolean
clawt_room_manager_remove(ClawtRoomManager *self, const gchar *room_id)
{
    ClawtRoom *room;
    guint i;

    g_return_val_if_fail(CLAWT_IS_ROOM_MANAGER(self), FALSE);
    g_return_val_if_fail(room_id != NULL, FALSE);

    room = g_hash_table_lookup(self->rooms, room_id);
    if (room == NULL)
        return FALSE;

    g_signal_handlers_disconnect_by_data(room, self);

    for (i = 0; i < self->order->len; i++) {
        if (g_strcmp0(g_ptr_array_index(self->order, i), room_id) == 0) {
            g_ptr_array_remove_index(self->order, i);
            break;
        }
    }

    /*
     * The transcript file is deliberately left behind.  Removing a room
     * from the fleet is a configuration change; destroying the record of
     * what was said in it is not, and is not recoverable.
     */
    g_signal_emit(self, signals[SIGNAL_ROOM_REMOVED], 0, room_id);
    g_hash_table_remove(self->rooms, room_id);

    return TRUE;
}

GPtrArray *
clawt_room_manager_rooms_for(ClawtRoomManager *self, const gchar *agent_id)
{
    g_autoptr(GPtrArray) all = NULL;
    GPtrArray *out;
    guint i;

    g_return_val_if_fail(CLAWT_IS_ROOM_MANAGER(self), NULL);

    out = g_ptr_array_new();
    all = clawt_room_manager_list(self);

    for (i = 0; i < all->len; i++) {
        ClawtRoom *room = g_ptr_array_index(all, i);

        if (clawt_room_has_member(room, agent_id))
            g_ptr_array_add(out, room);
    }

    return out;
}

guint
clawt_room_manager_load(ClawtRoomManager *self, ClawtConfig *config)
{
    g_autoptr(GPtrArray) declared = NULL;
    guint created = 0;
    guint i;

    g_return_val_if_fail(CLAWT_IS_ROOM_MANAGER(self), 0);
    g_return_val_if_fail(CLAWT_IS_CONFIG(config), 0);

    declared = clawt_config_get_rooms(config);

    for (i = 0; i < declared->len; i++) {
        ClawtRoomSpec *spec = g_ptr_array_index(declared, i);
        g_autoptr(GError) error = NULL;
        ClawtRoom *room;
        gsize j;

        room = clawt_room_manager_get(self, spec->id);

        if (room == NULL) {
            room = clawt_room_manager_create(self, spec->id, spec->name,
                                             &error);

            if (room == NULL) {
                /*
                 * One unusable room does not stop the fleet: the others
                 * still work, and the message says which one to fix.
                 */
                g_warning("rooms: %s", error->message);
                continue;
            }

            created++;
        }

        /*
         * Reconciled, not only added.
         *
         * This loop had no way to remove anybody, so taking a member out
         * of `clawtilla.yaml` and reloading left them in the room --
         * still receiving, still counted towards whether it is a group.
         * It only ever looked right because the config was read once at
         * start and a restart rebuilt the room from nothing.  Editing
         * members is a routine thing to do now, which turns that from a
         * latent bug into a constant one.
         */
        for (j = 0; spec->members != NULL && spec->members[j] != NULL; j++)
            clawt_room_add_member(room, spec->members[j]);

        {
            GPtrArray *current = clawt_room_get_members(room);
            g_autoptr(GPtrArray) stale =
                g_ptr_array_new_with_free_func(g_free);
            guint k;

            for (k = 0; k < current->len; k++) {
                const gchar *member = g_ptr_array_index(current, k);
                gboolean still_listed = FALSE;

                for (j = 0; spec->members != NULL &&
                            spec->members[j] != NULL; j++) {
                    if (g_strcmp0(spec->members[j], member) == 0) {
                        still_listed = TRUE;
                        break;
                    }
                }

                if (!still_listed)
                    g_ptr_array_add(stale, g_strdup(member));
            }

            /*
             * Collected first: removing during the walk would free the
             * strings being read.
             */
            for (k = 0; k < stale->len; k++)
                clawt_room_remove_member(room,
                                         g_ptr_array_index(stale, k));
        }

        /*
         * Only when the config actually said so.  Calling the setter
         * with a resolved default would mark the room as having been
         * told, and a standup that declares nothing would then inherit
         * `false` -- every member taking a turn on every remark, which
         * is the thing a mention rule exists to stop.
         */
        if (spec->require_mention_set)
            clawt_room_set_require_mention(room, spec->require_mention);

        clawt_room_set_max_hops(room, spec->max_hops);
        clawt_room_set_turn_timeout(room, spec->turn_timeout_seconds);
    }

    return created;
}

guint
clawt_room_manager_flush(ClawtRoomManager *self)
{
    g_return_val_if_fail(CLAWT_IS_ROOM_MANAGER(self), 0);

    /*
     * Nothing to do, and that is the point.
     *
     * Every message is on disk by the time clawt_room_append() returns,
     * so there is no in-memory copy for a shutdown to write out.  This
     * used to rewrite every transcript from memory, which is what made a
     * stale copy destructive: two daemons on one state directory each
     * flushed their own partial history over the other's, and four
     * messages of a real conversation were deleted that way.
     *
     * Kept rather than removed because a caller asking "is everything
     * persisted" deserves an answer, and the answer is yes.  It returns 0
     * because zero rooms needed writing.
     */
    return 0;
}

static void
clawt_room_manager_finalize(GObject *object)
{
    ClawtRoomManager *self = CLAWT_ROOM_MANAGER(object);

    g_clear_pointer(&self->rooms, g_hash_table_unref);
    g_clear_pointer(&self->order, g_ptr_array_unref);
    g_free(self->transcript_dir);

    G_OBJECT_CLASS(clawt_room_manager_parent_class)->finalize(object);
}

static void
clawt_room_manager_class_init(ClawtRoomManagerClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = clawt_room_manager_finalize;

    signals[SIGNAL_ROOM_ADDED] =
        g_signal_new("room-added", CLAWT_TYPE_ROOM_MANAGER,
                     G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE,
                     1, CLAWT_TYPE_ROOM);

    signals[SIGNAL_ROOM_REMOVED] =
        g_signal_new("room-removed", CLAWT_TYPE_ROOM_MANAGER,
                     G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE,
                     1, G_TYPE_STRING);
}

static void
clawt_room_manager_init(ClawtRoomManager *self)
{
    self->rooms = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                        g_object_unref);
    self->order = g_ptr_array_new_with_free_func(g_free);
}
