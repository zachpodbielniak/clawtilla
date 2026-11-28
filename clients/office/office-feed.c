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
/* pictures held at once; a larger fleet refetches the ones that fell off */
#define PORTRAIT_CACHE_MAX (48)
/* fetches in flight, so a roster cannot fill the render loop with replies */
#define PORTRAIT_FETCH_MAX (4)

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
    guint                 generation;   /* bumped by every refresh */
    guint                 list_seq;     /* next agent.list number; each reply carries one */
    guint                 list_applied; /* newest agent.list reply that was applied */
    GHashTable           *portraits;    /* id -> Portrait */
    GQueue                portrait_queue;
    guint                 portraits_inflight;
};

/* One agent's picture.  The bytes live here; the host is handed a copy. */
typedef struct {
    gchar   *id;
    gchar   *etag;
    gchar   *failed_etag;   /* decoded once, refused; not tried again until dirty */
    guint8  *rgba;
    gint     size;
    gboolean dirty;
    gboolean inflight;
    gboolean queued;
    guint    token;         /* an in-flight reply older than this is dropped */
    gint64   used;
} Portrait;

typedef struct {
    ClawtOfficeFeed *feed;
    GCancellable    *cancellable; /* own ref; the liveness test, not the feed's */
    gchar           *path;
    guint            generation;
} FileRequest;

/* One agent.list.  seq is list_seq at the moment it was sent. */
typedef struct {
    ClawtOfficeFeed *feed;
    GCancellable    *cancellable;
    guint            seq;
} ListRequest;

/*
 * A callback whose user_data was the feed.  free() cancels, then frees
 * the feed, and the reply still runs.  The cancellable here is a ref
 * this request owns, so the callback can see the cancel without reading
 * the feed.
 */
typedef struct {
    ClawtOfficeFeed *feed;
    GCancellable    *cancellable;
} FeedCall;

/* One character's files, fetched one after another; announced once all are here. */
typedef struct {
    ClawtOfficeFeed *feed;
    GCancellable    *cancellable;
    JsonObject      *character;
    gchar           *dir;
    GQueue           files;      /* "sheet" / "mesh": which member names the next file */
    guint            generation;
    gboolean         failed;
} SheetRequest;

static void fetch_next(ClawtOfficeFeed *self);
static void request_layout(ClawtOfficeFeed *self);
static void request_characters(ClawtOfficeFeed *self);
static void request_usage(ClawtOfficeFeed *self);
static void portrait_pump(ClawtOfficeFeed *self);

static FeedCall *
feed_call_new(ClawtOfficeFeed *self)
{
    FeedCall *call = g_new0(FeedCall, 1);

    call->feed = self;
    call->cancellable = g_object_ref(self->cancellable);
    return call;
}

static void
feed_call_free(FeedCall *call)
{
    g_object_unref(call->cancellable);
    g_free(call);
}

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
    ClawtOfficeFeed *self;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) node = NULL;
    JsonObject *reply;

    if (g_cancellable_is_cancelled(req->cancellable))
        goto done;
    self = req->feed;
    reply = finish(source, result, &node, &error);

    /*
     * A reply from a fetch a refresh has since started over (a replayed
     * office.changed at connect does this) is dropped: it would find the
     * new pass's queue and announce the world before its files are here.
     */
    if (req->generation != self->generation)
        goto done;
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
    g_object_unref(req->cancellable);
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
        request_characters(self);
        request_usage(self);
        return;
    }
    req = g_new0(FileRequest, 1);
    req->feed = self;
    req->cancellable = g_object_ref(self->cancellable);
    req->path = path;
    req->generation = self->generation;
    payload = clawt_ipc_payload_new("world",
                                    json_object_get_string_member_with_default(self->world,
                                                                               "id", ""),
                                    "path", path, NULL);
    request(self, "office.file", payload, on_file, req);
}

static void
on_world(GObject *source, GAsyncResult *result, gpointer user_data)
{
    FileRequest *req = user_data;
    ClawtOfficeFeed *self;
    guint generation;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) node = NULL;
    JsonObject *reply;
    JsonArray *assets;
    const gchar *id;
    guint i;

    if (g_cancellable_is_cancelled(req->cancellable)) {
        g_object_unref(req->cancellable);
        g_free(req);
        return;
    }
    self = req->feed;
    generation = req->generation;
    g_object_unref(req->cancellable);
    g_free(req);
    reply = finish(source, result, &node, &error);
    /* an older refresh's answer: a newer one is on its way */
    if (generation != self->generation)
        return;
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
    /* an adapter's source, when it is a file in the pack */
    if (json_object_has_member(reply, "adapter") &&
        JSON_NODE_HOLDS_OBJECT(json_object_get_member(reply, "adapter")))
        want(self, json_object_get_string_member_with_default(
            json_object_get_object_member(reply, "adapter"), "source", NULL));
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
    FeedCall *call = user_data;
    ClawtOfficeFeed *self;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) node = NULL;
    JsonObject *reply;

    if (g_cancellable_is_cancelled(call->cancellable)) {
        feed_call_free(call);
        return;
    }
    self = call->feed;
    feed_call_free(call);
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
    request(self, "office.layout", payload, on_layout, feed_call_new(self));
}

static gboolean
on_tick(gpointer user_data)
{
    request_layout(user_data);
    return G_SOURCE_CONTINUE;
}

/* ── Profile pictures ───────────────────────────────────────────── */

typedef struct {
    ClawtOfficeFeed *feed;
    GCancellable    *cancellable; /* own ref, checked before the feed is touched */
    gchar           *id;
    gchar           *etag;
    guint8          *raw;
    gsize            length;
    guint            token;
} PortraitJob;

static void
portrait_free(Portrait *portrait)
{
    if (portrait == NULL)
        return;
    g_free(portrait->id);
    g_free(portrait->etag);
    g_free(portrait->failed_etag);
    g_free(portrait->rgba);
    g_free(portrait);
}

static void
portrait_job_free(PortraitJob *job)
{
    g_clear_object(&job->cancellable);
    g_free(job->id);
    g_free(job->etag);
    g_free(job->raw);
    g_free(job);
}

static void
portrait_deliver(ClawtOfficeFeed *self, Portrait *portrait)
{
    if (self->funcs.portrait == NULL)
        return;
    self->funcs.portrait(portrait->id, portrait->rgba, portrait->size, portrait->etag,
                         self->user_data);
}

static void
portrait_evict(ClawtOfficeFeed *self)
{
    while (g_hash_table_size(self->portraits) > PORTRAIT_CACHE_MAX) {
        GHashTableIter iter;
        gpointer value;
        Portrait *oldest = NULL;

        g_hash_table_iter_init(&iter, self->portraits);
        while (g_hash_table_iter_next(&iter, NULL, &value)) {
            Portrait *portrait = value;

            if (oldest == NULL || portrait->used < oldest->used)
                oldest = portrait;
        }
        if (oldest == NULL)
            return;
        /*
         * Dropped from the host too, or a picture that fell out of the
         * cache would stay on screen after the bytes were freed.
         */
        if (oldest->rgba != NULL && self->funcs.portrait != NULL)
            self->funcs.portrait(oldest->id, NULL, 0, NULL, self->user_data);
        oldest->token++;
        g_hash_table_remove(self->portraits, oldest->id);
    }
}

static Portrait *
portrait_get(ClawtOfficeFeed *self, const gchar *id)
{
    Portrait *portrait = g_hash_table_lookup(self->portraits, id);

    if (portrait == NULL) {
        portrait = g_new0(Portrait, 1);
        portrait->id = g_strdup(id);
        portrait->used = g_get_monotonic_time();
        g_hash_table_insert(self->portraits, portrait->id, portrait);
        portrait_evict(self);
        portrait = g_hash_table_lookup(self->portraits, id);
    }
    if (portrait != NULL)
        portrait->used = g_get_monotonic_time();
    return portrait;
}

static void
portrait_queue(ClawtOfficeFeed *self, Portrait *portrait)
{
    if (portrait->queued || portrait->inflight)
        return;
    portrait->queued = TRUE;
    g_queue_push_tail(&self->portrait_queue, g_strdup(portrait->id));
    portrait_pump(self);
}

static void portrait_decoded(GObject *source, GAsyncResult *result, gpointer user_data);

/*
 * Decode on a worker.  job->length is the byte count going in and the
 * square's side coming out: the worker replaces it only after the
 * pointer has been handed to the task, and the task owns that pointer.
 */
static void
portrait_decode_worker(GTask *task, gpointer source, gpointer task_data,
                       GCancellable *cancellable)
{
    PortraitJob *job = task_data;
    guint8 *rgba = NULL;
    gint size = 0;
    GError *error = NULL;

    (void)source;
    (void)cancellable;
    if (!clawt_office_portrait_decode(job->raw, job->length, &rgba, &size, &error)) {
        g_task_return_error(task, error);
        return;
    }
    job->length = (gsize)size;
    g_task_return_pointer(task, rgba, g_free);
}

static void
portrait_decoded(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GTask *task = G_TASK(result);
    PortraitJob *job = g_task_get_task_data(task);
    ClawtOfficeFeed *self;
    Portrait *portrait;
    g_autoptr(GError) error = NULL;
    guint8 *rgba;

    (void)source;
    (void)user_data;
    rgba = g_task_propagate_pointer(task, &error);
    if (g_cancellable_is_cancelled(job->cancellable)) {
        g_free(rgba);
        return;
    }
    self = job->feed;
    portrait = g_hash_table_lookup(self->portraits, job->id);
    if (portrait != NULL)
        portrait->inflight = FALSE;
    if (portrait == NULL || portrait->token != job->token) {
        g_free(rgba);
        if (portrait != NULL && portrait->dirty)
            portrait_queue(self, portrait);
        portrait_pump(self);
        return;
    }
    portrait->dirty = FALSE;
    g_free(portrait->rgba);
    portrait->rgba = NULL;
    portrait->size = 0;
    if (rgba == NULL) {
        g_free(portrait->failed_etag);
        portrait->failed_etag = g_strdup(job->etag);
        portrait_deliver(self, portrait);
        portrait_pump(self);
        return;
    }
    g_free(portrait->failed_etag);
    portrait->failed_etag = NULL;
    g_free(portrait->etag);
    portrait->etag = g_strdup(job->etag);
    portrait->rgba = rgba;
    portrait->size = (gint)job->length;
    portrait->used = g_get_monotonic_time();
    portrait_deliver(self, portrait);
    portrait_pump(self);
}

static void
portrait_begin_decode(ClawtOfficeFeed *self, Portrait *portrait, const gchar *etag,
                      guint8 *raw, gsize length)
{
    PortraitJob *job = g_new0(PortraitJob, 1);
    GTask *task;

    job->feed = self;
    job->cancellable = g_object_ref(self->cancellable);
    job->id = g_strdup(portrait->id);
    job->etag = g_strdup(etag);
    job->raw = raw;
    job->length = length;
    job->token = portrait->token;
    g_main_context_push_thread_default(self->context);
    task = g_task_new(NULL, self->cancellable, portrait_decoded, NULL);
    g_task_set_task_data(task, job, (GDestroyNotify)portrait_job_free);
    g_task_run_in_thread(task, portrait_decode_worker);
    g_object_unref(task);
    g_main_context_pop_thread_default(self->context);
}

static void
on_avatar(GObject *source, GAsyncResult *result, gpointer user_data)
{
    PortraitJob *req = user_data;
    ClawtOfficeFeed *self;
    Portrait *portrait;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) node = NULL;
    JsonObject *reply;
    const gchar *encoded;
    const gchar *etag;
    const gchar *mime;
    g_autofree guint8 *raw = NULL;
    gsize length = 0;

    if (g_cancellable_is_cancelled(req->cancellable)) {
        portrait_job_free(req);
        return;
    }
    self = req->feed;
    self->portraits_inflight--;
    portrait = g_hash_table_lookup(self->portraits, req->id);
    if (portrait != NULL)
        portrait->inflight = FALSE;
    if (portrait == NULL || portrait->token != req->token) {
        if (portrait != NULL && portrait->dirty)
            portrait_queue(self, portrait);
        portrait_job_free(req);
        portrait_pump(self);
        return;
    }

    reply = finish(source, result, &node, &error);
    encoded = reply != NULL
        ? json_object_get_string_member_with_default(reply, "base64", NULL) : NULL;
    etag = reply != NULL
        ? json_object_get_string_member_with_default(reply, "etag", NULL) : NULL;
    mime = reply != NULL
        ? json_object_get_string_member_with_default(reply, "mime", NULL) : NULL;
    if (encoded == NULL || etag == NULL) {
        /* gone, or a transport error: drop what we were showing and do not spin */
        if (error == NULL || error->code == CLAWT_ERROR_NOT_FOUND) {
            g_free(portrait->rgba);
            portrait->rgba = NULL;
            portrait->size = 0;
            g_free(portrait->etag);
            portrait->etag = NULL;
            portrait->dirty = FALSE;
            portrait_deliver(self, portrait);
        } else {
            portrait->dirty = TRUE;
        }
        portrait_job_free(req);
        portrait_pump(self);
        return;
    }
    if (portrait->etag != NULL && g_strcmp0(portrait->etag, etag) == 0 &&
        portrait->rgba != NULL) {
        portrait->dirty = FALSE;
        portrait_job_free(req);
        portrait_pump(self);
        return;
    }
    if (portrait->failed_etag != NULL && g_strcmp0(portrait->failed_etag, etag) == 0) {
        portrait->dirty = FALSE;
        portrait_job_free(req);
        portrait_pump(self);
        return;
    }
    raw = g_base64_decode(encoded, &length);
    if (raw == NULL || length == 0 || length > CLAWT_OFFICE_PORTRAIT_MAX_BYTES ||
        g_strcmp0(mime, "image/png") != 0) {
        g_free(portrait->rgba);
        portrait->rgba = NULL;
        portrait->size = 0;
        g_free(portrait->failed_etag);
        portrait->failed_etag = g_strdup(etag);
        portrait->dirty = FALSE;
        portrait_deliver(self, portrait);
        portrait_job_free(req);
        portrait_pump(self);
        return;
    }
    portrait->inflight = TRUE;
    portrait_begin_decode(self, portrait, etag, g_steal_pointer(&raw), length);
    portrait_job_free(req);
}

static void
portrait_pump(ClawtOfficeFeed *self)
{
    while (self->portraits_inflight < PORTRAIT_FETCH_MAX) {
        g_autofree gchar *id = g_queue_pop_head(&self->portrait_queue);
        Portrait *portrait;
        PortraitJob *req;

        if (id == NULL)
            return;
        portrait = g_hash_table_lookup(self->portraits, id);
        if (portrait == NULL)
            continue;
        portrait->queued = FALSE;
        if (!portrait->dirty || portrait->inflight)
            continue;
        portrait->inflight = TRUE;
        portrait->used = g_get_monotonic_time();
        self->portraits_inflight++;
        req = g_new0(PortraitJob, 1);
        req->feed = self;
        req->cancellable = g_object_ref(self->cancellable);
        req->id = g_strdup(portrait->id);
        req->token = portrait->token;
        request(self, "agent.avatar",
                clawt_ipc_payload_new("agent", portrait->id, NULL),
                on_avatar, req);
    }
}

static void
portrait_consider(ClawtOfficeFeed *self, const gchar *id, gboolean has_avatar)
{
    Portrait *portrait = g_hash_table_lookup(self->portraits, id);

    if (!has_avatar) {
        if (portrait == NULL)
            return;
        portrait->token++;
        if (portrait->rgba != NULL && self->funcs.portrait != NULL)
            self->funcs.portrait(id, NULL, 0, NULL, self->user_data);
        g_hash_table_remove(self->portraits, id);
        return;
    }
    portrait = portrait_get(self, id);
    if (portrait == NULL)
        return;
    if (!portrait->dirty && portrait->rgba != NULL)
        return;
    if (!portrait->dirty && portrait->failed_etag != NULL)
        return;
    portrait->dirty = TRUE;
    portrait_queue(self, portrait);
}

static void
portrait_mark_dirty(ClawtOfficeFeed *self, const gchar *id)
{
    Portrait *portrait;

    if (id == NULL) {
        GHashTableIter iter;
        gpointer value;

        g_hash_table_iter_init(&iter, self->portraits);
        while (g_hash_table_iter_next(&iter, NULL, &value)) {
            portrait = value;
            portrait->dirty = TRUE;
            portrait->token++;
            if (!portrait->inflight)
                portrait_queue(self, portrait);
        }
        return;
    }
    portrait = g_hash_table_lookup(self->portraits, id);
    if (portrait == NULL)
        return;
    portrait->dirty = TRUE;
    portrait->token++;
    if (!portrait->inflight)
        portrait_queue(self, portrait);
}

static void
on_agents(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ListRequest *req = user_data;
    ClawtOfficeFeed *self;
    guint seq;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) node = NULL;
    JsonObject *reply;
    JsonArray *agents;
    guint i;

    if (g_cancellable_is_cancelled(req->cancellable)) {
        g_object_unref(req->cancellable);
        g_free(req);
        return;
    }
    self = req->feed;
    seq = req->seq;
    g_object_unref(req->cancellable);
    g_free(req);
    reply = finish(source, result, &node, &error);

    /*
     * Two lists can be in flight: the one from connect, and the one
     * agent.changed sends after a picture is stored.  The daemon answers
     * them in whatever order the socket happens to.  An older reply that
     * lands second still says has_avatar is false and would drop the
     * picture the newer reply has already drawn.  The number is the
     * request's, so a reply older than the newest one applied changes
     * nothing: no colour, no look, no portrait.
     */
    if (seq < self->list_applied)
        return;
    if (reply != NULL)
        self->list_applied = seq;

    agents = reply != NULL && json_object_has_member(reply, "agents")
        ? json_object_get_array_member(reply, "agents") : NULL;
    for (i = 0; agents != NULL && i < json_array_get_length(agents); i++) {
        JsonObject *agent = json_array_get_object_element(agents, i);
        const gchar *id = json_object_get_string_member_with_default(agent, "id", NULL);
        gboolean has_avatar = json_object_get_boolean_member_with_default(agent, "has_avatar",
                                                                          FALSE);

        if (id == NULL)
            continue;
        if (self->funcs.color != NULL)
            self->funcs.color(id,
                              json_object_get_string_member_with_default(agent, "color", NULL),
                              self->user_data);
        if (self->funcs.look != NULL)
            self->funcs.look(id,
                             json_object_get_string_member_with_default(agent, "name", NULL),
                             json_object_get_string_member_with_default(agent, "face", NULL),
                             has_avatar, self->user_data);
        portrait_consider(self, id, has_avatar);
    }
}

/* ── Characters ─────────────────────────────────────────────────── */

static void fetch_character_file(SheetRequest *req);

static void
sheet_request_free(SheetRequest *req)
{
    g_clear_object(&req->cancellable);
    json_object_unref(req->character);
    g_free(req->dir);
    g_queue_clear_full(&req->files, g_free);
    g_free(req);
}

/*
 * A character's file, fetched over office.character_file into the cache
 * and checked against the checksum its manifest recorded: a file that
 * does not match fails the character, which is then not drawn, rather
 * than drawn wrong.
 */
static void
on_character_file(GObject *source, GAsyncResult *result, gpointer user_data)
{
    SheetRequest *req = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) node = NULL;
    g_autofree gchar *member = g_queue_pop_head(&req->files);
    g_autofree gchar *sum_member = g_strconcat(member, "_sha256", NULL);
    const gchar *relative = json_object_get_string_member_with_default(req->character, member,
                                                                       NULL);
    const gchar *want = json_object_get_string_member_with_default(req->character, sum_member,
                                                                   NULL);
    JsonObject *reply;
    const gchar *encoded;
    g_autofree guchar *data = NULL;
    g_autofree gchar *sum = NULL;
    g_autofree gchar *local = NULL;
    g_autofree gchar *parent = NULL;
    gsize length = 0;

    if (g_cancellable_is_cancelled(req->cancellable)) {
        sheet_request_free(req);
        return;
    }
    reply = finish(source, result, &node, &error);
    encoded = reply != NULL ? json_object_get_string_member_with_default(reply, "base64", NULL)
                            : NULL;
    if (encoded == NULL) {
        req->failed = TRUE;
    } else {
        data = g_base64_decode(encoded, &length);
        sum = g_compute_checksum_for_data(G_CHECKSUM_SHA256, data, length);
        if (want != NULL && g_ascii_strcasecmp(want, sum) != 0) {
            g_warning("office: %s's %s does not match its manifest; not drawn",
                      json_object_get_string_member_with_default(req->character, "agent", "?"),
                      member);
            req->failed = TRUE;
        } else {
            local = g_build_filename(req->dir, relative, NULL);
            parent = g_path_get_dirname(local);
            g_mkdir_with_parents(parent, 0700);
            if (!g_file_set_contents(local, (const gchar *)data, (gssize)length, NULL))
                req->failed = TRUE;
        }
    }
    fetch_character_file(req);
}

static void
fetch_character_file(SheetRequest *req)
{
    ClawtOfficeFeed *self = req->feed;
    const gchar *member = g_queue_peek_head(&req->files);
    const gchar *agent = json_object_get_string_member(req->character, "agent");

    if (member == NULL) {
        if (!req->failed && req->generation == self->generation && self->funcs.character != NULL)
            self->funcs.character(agent, req->dir, req->character, self->user_data);
        sheet_request_free(req);
        return;
    }
    request(self, "office.character_file",
            clawt_ipc_payload_new("agent", agent, "path",
                                  json_object_get_string_member(req->character, member), NULL),
            on_character_file, req);
}

static void
on_characters(GObject *source, GAsyncResult *result, gpointer user_data)
{
    FeedCall *call = user_data;
    ClawtOfficeFeed *self;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) node = NULL;
    JsonObject *reply;
    JsonArray *list;
    guint i;

    if (g_cancellable_is_cancelled(call->cancellable)) {
        feed_call_free(call);
        return;
    }
    self = call->feed;
    feed_call_free(call);
    if (self->funcs.character == NULL)
        return;
    reply = finish(source, result, &node, &error);
    list = reply != NULL && json_object_has_member(reply, "characters")
        ? json_object_get_array_member(reply, "characters") : NULL;
    for (i = 0; list != NULL && i < json_array_get_length(list); i++) {
        JsonObject *c = json_array_get_object_element(list, i);
        const gchar *agent = json_object_get_string_member_with_default(c, "agent", NULL);
        static const gchar *const members[] = { "sheet", "mesh", NULL };
        SheetRequest *req;
        guint m;

        if (agent == NULL || !clawt_office_pack_path_is_safe(agent) || strchr(agent, '/') != NULL)
            continue;
        req = g_new0(SheetRequest, 1);
        req->feed = self;
        req->cancellable = g_object_ref(self->cancellable);
        req->character = json_object_ref(c);
        req->generation = self->generation;
        req->dir = g_build_filename(g_get_user_cache_dir(), "clawtilla", "office",
                                    "characters", agent, NULL);
        g_queue_init(&req->files);
        for (m = 0; members[m] != NULL; m++) {
            const gchar *path = json_object_get_string_member_with_default(c, members[m], NULL);

            if (path != NULL && clawt_office_pack_path_is_safe(path))
                g_queue_push_tail(&req->files, g_strdup(members[m]));
        }
        fetch_character_file(req);
    }
}

static void
request_characters(ClawtOfficeFeed *self)
{
    request(self, "office.characters", NULL, on_characters, feed_call_new(self));
}

/* ── The usage board ──────────────────────────────────────────────── */

static void
on_usage(GObject *source, GAsyncResult *result, gpointer user_data)
{
    FeedCall *call = user_data;
    ClawtOfficeFeed *self;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) node = NULL;
    JsonObject *reply;

    if (g_cancellable_is_cancelled(call->cancellable)) {
        feed_call_free(call);
        return;
    }
    self = call->feed;
    feed_call_free(call);
    reply = finish(source, result, &node, &error);
    if (reply != NULL && self->funcs.usage != NULL)
        self->funcs.usage(reply, self->user_data);
}

/*
 * As it stands, never with `refresh`: reading an allowance spawns the
 * provider's CLI, and a wall display left open all day must not do that
 * once a minute.  The Fleet page refreshes; the board shows the result.
 */
static void
request_usage(ClawtOfficeFeed *self)
{
    if (self->funcs.usage != NULL)
        request(self, "usage.providers", NULL, on_usage, feed_call_new(self));
}

/* The next agent.list.  Its reply is ignored once a newer one has landed. */
static void
request_agent_list(ClawtOfficeFeed *self)
{
    ListRequest *req = g_new0(ListRequest, 1);

    req->feed = self;
    req->cancellable = g_object_ref(self->cancellable);
    req->seq = ++self->list_seq;
    request(self, "agent.list", NULL, on_agents, req);
}

/* ── Life cycle ─────────────────────────────────────────────────── */

void
clawt_office_feed_refresh(ClawtOfficeFeed *self)
{
    JsonNode *payload = NULL;

    self->ready = FALSE;
    self->generation++;
    if (self->world_id != NULL)
        payload = clawt_ipc_payload_new("world", self->world_id, NULL);
    {
        FileRequest *req = g_new0(FileRequest, 1);

        req->feed = self;
        req->cancellable = g_object_ref(self->cancellable);
        req->generation = self->generation;
        request(self, "office.world", payload, on_world, req);
    }
    request_agent_list(self);
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
    self->portraits = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
                                            (GDestroyNotify)portrait_free);
    g_queue_init(&self->pending);
    g_queue_init(&self->portrait_queue);

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
        /* a pack, a world choice or a character: read it all again */
        clawt_office_feed_refresh(self);
    else if (g_strcmp0(kind, "usage.providers_changed") == 0)
        request_usage(self);
    else if (g_strcmp0(kind, "agent.created") == 0 ||
             g_strcmp0(kind, "agent.changed") == 0 ||
             (kind != NULL && g_str_has_prefix(kind, "agent.avatar"))) {
        /* a new picture shows up without restarting the office */
        portrait_mark_dirty(self, clawt_event_get_subject(event));
        request_agent_list(self);
    }
}

void
clawt_office_feed_free(ClawtOfficeFeed *self)
{
    if (self == NULL)
        return;
    /*
     * Cancel first.  Every reply holds its own ref of this cancellable
     * and reads that, not the feed, so the free below is not a
     * use-after-free when the callback runs later.  The timer is not one
     * of those replies: destroying it drops on_tick before the struct goes.
     */
    g_cancellable_cancel(self->cancellable);
    if (self->timer != NULL) {
        g_source_destroy(self->timer);
        g_source_unref(self->timer);
    }
    g_queue_clear_full(&self->pending, g_free);
    g_queue_clear_full(&self->portrait_queue, g_free);
    g_hash_table_unref(self->asked);
    g_hash_table_unref(self->portraits);
    g_clear_pointer(&self->world, json_object_unref);
    g_object_unref(self->cancellable);
    g_object_unref(self->client);
    g_main_context_unref(self->context);
    g_free(self->world_id);
    g_free(self->pack_dir);
    g_free(self);
}
