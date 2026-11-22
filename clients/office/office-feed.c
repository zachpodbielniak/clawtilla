/*
 * office-feed.c - The daemon's side of the drawn office
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The pack is fetched file by file through office.file rather than read
 * from the daemon's disk: a renderer on a laptop draws the workstation's
 * office, and the daemon is the one that knows where its packs are.
 * Fetched into the user's cache, one directory per world id, and every
 * path is checked with the same rule the pack loader uses before it is
 * written there -- a daemon is trusted, a path it relays is still data.
 */

#include "office-feed.h"

#include <string.h>

#define LAYOUT_EVERY_MS (1000)

struct _ClawtOfficeFeed {
    ClawtClient          *client;
    GMainContext         *context;
    GCancellable         *cancellable;
    gchar                *world_id;
    ClawtOfficeFeedFuncs  funcs;
    gpointer              user_data;
    JsonObject           *world;
    gchar                *pack_dir;
    GQueue                pending;      /* relative paths still to fetch */
    GHashTable           *asked;        /* paths already queued */
    GSource              *timer;
    gboolean              layout_in_flight;
    gboolean              ready;
};

typedef struct {
    ClawtOfficeFeed *feed;
    gchar           *path;
} FileRequest;

static void fetch_next(ClawtOfficeFeed *self);
static void request_layout(ClawtOfficeFeed *self);

static void
say(ClawtOfficeFeed *self, const gchar *text)
{
    if (self->funcs.status != NULL)
        self->funcs.status(text, self->user_data);
}

/* Every request on the host's context, cancelled with the feed. */
static void
request(ClawtOfficeFeed *self, const gchar *kind, JsonNode *payload,
        GAsyncReadyCallback callback, gpointer data)
{
    g_main_context_push_thread_default(self->context);
    clawt_client_request_async(self->client, kind, payload, self->cancellable, callback,
                               data);
    g_main_context_pop_thread_default(self->context);
}

static JsonObject *
finish(GObject *source, GAsyncResult *result, JsonNode **out_node, GError **error)
{
    JsonNode *reply = clawt_client_request_finish(CLAWT_CLIENT(source), result, error);

    *out_node = reply;
    return reply != NULL ? clawt_ipc_reply_object(reply) : NULL;
}

/* ── The pack ───────────────────────────────────────────────────── */

static void
want(ClawtOfficeFeed *self, const gchar *path)
{
    if (path == NULL || !clawt_office_pack_path_is_safe(path) ||
        g_hash_table_contains(self->asked, path))
        return;
    g_hash_table_add(self->asked, g_strdup(path));
    g_queue_push_tail(&self->pending, g_strdup(path));
}

/* A Tiled map names its tilesets' images; they are part of the pack too. */
static void
want_map_images(ClawtOfficeFeed *self, const gchar *map_path)
{
    g_autofree gchar *full = g_build_filename(self->pack_dir, map_path, NULL);
    g_autofree gchar *dir = g_path_get_dirname(map_path);
    g_autoptr(JsonParser) parser = json_parser_new();
    JsonObject *root;
    JsonArray *tilesets;
    guint i;

    if (!json_parser_load_from_file(parser, full, NULL) ||
        !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
        return;
    root = json_node_get_object(json_parser_get_root(parser));
    tilesets = json_object_has_member(root, "tilesets")
        ? json_object_get_array_member(root, "tilesets") : NULL;
    for (i = 0; tilesets != NULL && i < json_array_get_length(tilesets); i++) {
        JsonObject *ts = json_array_get_object_element(tilesets, i);
        const gchar *image = json_object_get_string_member_with_default(ts, "image", NULL);
        g_autofree gchar *path = NULL;

        if (image == NULL)
            continue;
        path = g_strcmp0(dir, ".") == 0 ? g_strdup(image)
                                        : g_build_filename(dir, image, NULL);
        want(self, path);
    }
}

static void
on_file(GObject *source, GAsyncResult *result, gpointer user_data)
{
    FileRequest *req = user_data;
    ClawtOfficeFeed *self = req->feed;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) node = NULL;
    JsonObject *reply;

    if (g_cancellable_is_cancelled(self->cancellable) ||
        g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        goto done;
    reply = finish(source, result, &node, &error);
    if (reply == NULL) {
        g_autofree gchar *line = g_strdup_printf("Could not read %s from the daemon: %s",
                                                 req->path,
                                                 error != NULL ? error->message : "no reply");

        say(self, line);
    } else {
        const gchar *encoded = json_object_get_string_member_with_default(reply, "base64",
                                                                          NULL);
        g_autofree gchar *full = g_build_filename(self->pack_dir, req->path, NULL);
        g_autofree gchar *parent = g_path_get_dirname(full);
        g_autofree guchar *data = NULL;
        gsize length = 0;

        if (encoded != NULL) {
            data = g_base64_decode(encoded, &length);
            g_mkdir_with_parents(parent, 0700);
            if (!g_file_set_contents(full, (const gchar *)data, (gssize)length, &error))
                g_warning("office: could not cache %s: %s", full, error->message);
            else if (g_str_has_suffix(req->path, ".tmj"))
                want_map_images(self, req->path);
        }
    }
    fetch_next(self);

done:
    g_free(req->path);
    g_free(req);
}

static void
fetch_next(ClawtOfficeFeed *self)
{
    gchar *path = g_queue_pop_head(&self->pending);
    FileRequest *req;
    JsonNode *payload;

    if (path == NULL) {
        /* the whole pack is here: draw it, then keep the layout coming */
        if (self->funcs.world != NULL)
            self->funcs.world(self->pack_dir, self->world, self->user_data);
        self->ready = TRUE;
        request_layout(self);
        return;
    }
    req = g_new0(FileRequest, 1);
    req->feed = self;
    req->path = path;
    payload = clawt_ipc_payload_new("world",
                                    json_object_get_string_member_with_default(self->world,
                                                                               "id", ""),
                                    "path", path, NULL);
    request(self, "office.file", payload, on_file, req);
}

static void
on_world(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ClawtOfficeFeed *self = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) node = NULL;
    JsonObject *reply;
    JsonArray *assets;
    const gchar *id;
    guint i;

    if (g_cancellable_is_cancelled(self->cancellable))
        return;
    reply = finish(source, result, &node, &error);
    if (reply == NULL) {
        g_autofree gchar *line = g_strdup_printf("The daemon did not give the world: %s",
            error != NULL ? error->message : "no reply");

        say(self, line);
        return;
    }
    id = json_object_get_string_member_with_default(reply, "id", NULL);
    if (id == NULL || !clawt_office_pack_path_is_safe(id) || strchr(id, '/') != NULL) {
        say(self, "The daemon named a world that cannot be a directory.");
        return;
    }
    g_clear_pointer(&self->world, json_object_unref);
    self->world = json_object_ref(reply);
    g_free(self->pack_dir);
    self->pack_dir = g_build_filename(g_get_user_cache_dir(), "clawtilla", "office", id, NULL);
    g_mkdir_with_parents(self->pack_dir, 0700);

    g_queue_clear_full(&self->pending, g_free);
    g_hash_table_remove_all(self->asked);
    want(self, json_object_get_string_member_with_default(reply, "map_2d", NULL));
    want(self, json_object_get_string_member_with_default(reply, "level_3d", NULL));
    want(self, "office.yaml");
    assets = json_object_has_member(reply, "assets")
        ? json_object_get_array_member(reply, "assets") : NULL;
    for (i = 0; assets != NULL && i < json_array_get_length(assets); i++)
        want(self, json_object_get_string_member_with_default(
            json_array_get_object_element(assets, i), "path", NULL));
    say(self, "Fetching the world...");
    fetch_next(self);
}

/* ── Who sits where ─────────────────────────────────────────────── */

static void
on_layout(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ClawtOfficeFeed *self = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) node = NULL;
    JsonObject *reply;

    if (g_cancellable_is_cancelled(self->cancellable))
        return;
    self->layout_in_flight = FALSE;
    reply = finish(source, result, &node, &error);
    if (reply == NULL)
        return;
    if (!json_object_get_boolean_member_with_default(reply, "enabled", TRUE)) {
        say(self, json_object_get_string_member_with_default(reply, "why",
                                                             "The office is off."));
        return;
    }
    if (self->funcs.layout != NULL)
        self->funcs.layout(reply, self->user_data);
}

static void
request_layout(ClawtOfficeFeed *self)
{
    JsonNode *payload = NULL;

    if (!self->ready || self->layout_in_flight)
        return;
    self->layout_in_flight = TRUE;
    if (self->world_id != NULL)
        payload = clawt_ipc_payload_new("world", self->world_id, NULL);
    request(self, "office.layout", payload, on_layout, self);
}

static gboolean
on_tick(gpointer user_data)
{
    request_layout(user_data);
    return G_SOURCE_CONTINUE;
}

static void
on_agents(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ClawtOfficeFeed *self = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) node = NULL;
    JsonObject *reply;
    JsonArray *agents;
    guint i;

    if (g_cancellable_is_cancelled(self->cancellable))
        return;
    reply = finish(source, result, &node, &error);
    agents = reply != NULL && json_object_has_member(reply, "agents")
        ? json_object_get_array_member(reply, "agents") : NULL;
    for (i = 0; agents != NULL && i < json_array_get_length(agents); i++) {
        JsonObject *agent = json_array_get_object_element(agents, i);

        if (self->funcs.color != NULL)
            self->funcs.color(json_object_get_string_member_with_default(agent, "id", NULL),
                              json_object_get_string_member_with_default(agent, "color",
                                                                         NULL),
                              self->user_data);
    }
}

/* ── Life cycle ─────────────────────────────────────────────────── */

void
clawt_office_feed_refresh(ClawtOfficeFeed *self)
{
    JsonNode *payload = NULL;

    self->ready = FALSE;
    if (self->world_id != NULL)
        payload = clawt_ipc_payload_new("world", self->world_id, NULL);
    request(self, "office.world", payload, on_world, self);
    request(self, "agent.list", NULL, on_agents, self);
}

ClawtOfficeFeed *
clawt_office_feed_new(ClawtClient                *client,
                      GMainContext               *context,
                      const gchar                *world_id,
                      const ClawtOfficeFeedFuncs *funcs,
                      gpointer                    user_data)
{
    ClawtOfficeFeed *self = g_new0(ClawtOfficeFeed, 1);

    self->client = g_object_ref(client);
    self->context = g_main_context_ref(context);
    self->cancellable = g_cancellable_new();
    self->world_id = g_strdup(world_id);
    self->funcs = *funcs;
    self->user_data = user_data;
    self->asked = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_queue_init(&self->pending);

    /* the timer on the host's context, named where it is attached */
    self->timer = g_timeout_source_new(LAYOUT_EVERY_MS);
    g_source_set_callback(self->timer, on_tick, self, NULL);
    g_source_attach(self->timer, self->context);

    clawt_office_feed_refresh(self);
    return self;
}

void
clawt_office_feed_event(ClawtOfficeFeed *self, ClawtEvent *event)
{
    const gchar *kind = clawt_event_get_kind(event);

    if (g_strcmp0(kind, "office.presence") == 0)
        request_layout(self);
    else if (g_strcmp0(kind, "office.changed") == 0)
        clawt_office_feed_refresh(self);
    else if (g_strcmp0(kind, "agent.created") == 0 || g_strcmp0(kind, "agent.changed") == 0)
        request(self, "agent.list", NULL, on_agents, self);
}

void
clawt_office_feed_free(ClawtOfficeFeed *self)
{
    if (self == NULL)
        return;
    /* replies in flight see the cancellable and touch nothing else */
    g_cancellable_cancel(self->cancellable);
    if (self->timer != NULL) {
        g_source_destroy(self->timer);
        g_source_unref(self->timer);
    }
    g_queue_clear_full(&self->pending, g_free);
    g_hash_table_unref(self->asked);
    g_clear_pointer(&self->world, json_object_unref);
    g_object_unref(self->cancellable);
    g_object_unref(self->client);
    g_main_context_unref(self->context);
    g_free(self->world_id);
    g_free(self->pack_dir);
    g_free(self);
}
