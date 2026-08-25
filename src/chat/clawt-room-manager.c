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
save_room(ClawtRoomManager *self, ClawtRoom *room)
{
    g_autofree gchar *path = NULL;
    g_autoptr(GPtrArray) history = NULL;
    g_autoptr(GString) out = NULL;
    g_autoptr(GError) error = NULL;
    guint i;

    path = transcript_path(self, clawt_room_get_id(room));
    if (path == NULL)
        return;

    history = clawt_room_get_history(room, 0);
    out = g_string_new(NULL);

    for (i = 0; i < history->len; i++) {
        ClawtMessage *message = g_ptr_array_index(history, i);
        g_autoptr(JsonBuilder) builder = json_builder_new();
        g_autoptr(JsonGenerator) generator = json_generator_new();
        g_autoptr(JsonNode) root = NULL;
        g_autofree gchar *line = NULL;

        json_builder_begin_object(builder);

        json_builder_set_member_name(builder, "id");
        json_builder_add_string_value(builder,
                                      clawt_message_get_id(message));
        json_builder_set_member_name(builder, "sender");
        json_builder_add_string_value(builder,
                                      clawt_message_get_sender_id(message));

        if (clawt_message_get_sender_name(message) != NULL) {
            json_builder_set_member_name(builder, "sender_name");
            json_builder_add_string_value(
                builder, clawt_message_get_sender_name(message));
        }

        json_builder_set_member_name(builder, "body");
        json_builder_add_string_value(builder,
                                      clawt_message_get_body(message));
        json_builder_set_member_name(builder, "ts");
        json_builder_add_int_value(builder,
                                   clawt_message_get_timestamp(message));
        json_builder_set_member_name(builder, "depth");
        json_builder_add_int_value(builder,
                                   clawt_message_get_depth(message));

        if (clawt_message_get_task_id(message) != NULL) {
            json_builder_set_member_name(builder, "task_id");
            json_builder_add_string_value(
                builder, clawt_message_get_task_id(message));
        }

        if (clawt_message_get_parent_id(message) != NULL) {
            json_builder_set_member_name(builder, "parent_id");
            json_builder_add_string_value(
                builder, clawt_message_get_parent_id(message));
        }

        json_builder_end_object(builder);

        root = json_builder_get_root(builder);
        json_generator_set_root(generator, root);
        line = json_generator_to_data(generator, NULL);

        g_string_append(out, line);
        g_string_append_c(out, '\n');
    }

    if (!clawt_ensure_dir(self->transcript_dir, 0700, &error) ||
        !clawt_write_file_atomic(path, out->str, (gssize)out->len, 0600,
                                 FALSE, &error))
        g_warning("could not save the transcript for %s: %s",
                  clawt_room_get_id(room), error->message);
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

        if (json_object_has_member(object, "ts"))
            clawt_message_set_timestamp(
                message, json_object_get_int_member(object, "ts"));

        if (json_object_has_member(object, "depth"))
            clawt_message_set_depth(
                message, (gint)json_object_get_int_member(object, "depth"));

        if (json_object_has_member(object, "task_id"))
            clawt_message_set_task_id(
                message, json_object_get_string_member(object, "task_id"));

        if (json_object_has_member(object, "parent_id"))
            clawt_message_set_parent_id(
                message, json_object_get_string_member(object, "parent_id"));

        clawt_room_append(room, message, NULL);
    }
}

static void
on_message_added(ClawtRoom *room, ClawtMessage *message, gpointer user_data)
{
    ClawtRoomManager *self = user_data;

    (void)message;

    save_room(self, room);
}

static ClawtRoom *
insert_room(ClawtRoomManager *self, ClawtRoom *room)
{
    const gchar *room_id = clawt_room_get_id(room);

    g_hash_table_replace(self->rooms, g_strdup(room_id), room);
    g_ptr_array_add(self->order, g_strdup(room_id));

    load_room(self, room);

    /*
     * Saved on every message rather than on shutdown.  A daemon that is
     * killed rather than stopped is the ordinary case for a service, and a
     * transcript that only survives a clean exit is not a transcript.
     */
    if (self->transcript_dir != NULL)
        g_signal_connect(room, "message-added",
                         G_CALLBACK(on_message_added), self);

    g_signal_emit(self, signals[SIGNAL_ROOM_ADDED], 0, room);

    return room;
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

    room = clawt_room_new(room_id, name);

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

        for (j = 0; spec->members != NULL && spec->members[j] != NULL; j++)
            clawt_room_add_member(room, spec->members[j]);

        clawt_room_set_require_mention(room, spec->require_mention);
        clawt_room_set_max_hops(room, spec->max_hops);
    }

    return created;
}

guint
clawt_room_manager_flush(ClawtRoomManager *self)
{
    g_autoptr(GPtrArray) rooms = NULL;
    guint i;

    g_return_val_if_fail(CLAWT_IS_ROOM_MANAGER(self), 0);

    if (self->transcript_dir == NULL)
        return 0;

    rooms = clawt_room_manager_list(self);

    for (i = 0; i < rooms->len; i++)
        save_room(self, g_ptr_array_index(rooms, i));

    return rooms->len;
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
