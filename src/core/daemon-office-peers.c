/*
 * daemon-office-peers.c - Other fleets' people, visiting this office
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Federation, the read-only half: `office.peers` names other daemons --
 * a socket on this machine, or a tailnet address paired once with
 * `clawtilla office peer-pair` -- and this daemon asks each for its
 * `office.snapshot` and draws the people in it as visitors.  The
 * invariants this rests on were laid down before it was built
 * (docs/office.org#federation): every participant carries its fleet's
 * id, and a fleet publishes only its own people -- office.snapshot never
 * includes a peer's -- so two fleets that peer with each other do not
 * echo each other's visitors back and forth.
 *
 * A peer's answer is somebody else's input.  It is bounded (a count, a
 * length for every string, an id spelling), it can only ever become a
 * participant of kind `external` under the peer's own name, and nothing
 * in it reaches anything that acts: it is drawn, and that is all.
 *
 * Nothing here runs at daemon start.  The first `office.layout` -- a
 * client drawing the office -- starts the polling; a daemon nobody looks
 * at never dials a peer, and `make test` dials only the unix sockets a
 * test gives it.
 */

#include "clawtilla.h"
#include "clawt-daemon-private.h"

#include <glib/gstdio.h>
#include <string.h>

#define PEER_MAX_PARTICIPANTS (200)
#define PEER_MAX_TEXT (64)
#define PEER_STALE_POLLS (3)

typedef struct {
    ClawtDaemon *daemon;          /* not owned */
    gchar       *name;
    gchar       *target;          /* a socket path, or host:port */
    ClawtClient *client;
    gboolean     connecting;
    gboolean     in_flight;
    gchar       *fleet;           /* theirs, as they said it */
    JsonArray   *participants;    /* sanitized, ready to merge */
    gint64       fetched_at;      /* Unix seconds of the last good answer */
    gchar       *error;
} Peer;

static void
peer_free(Peer *peer)
{
    if (peer->client != NULL)
        clawt_client_disconnect(peer->client);
    g_clear_object(&peer->client);
    g_free(peer->name);
    g_free(peer->target);
    g_free(peer->fleet);
    g_clear_pointer(&peer->participants, json_array_unref);
    g_free(peer->error);
    g_free(peer);
}

/* The token a tailnet peer is reached with, kept as a connector credential is. */
static gchar *
token_path(ClawtDaemon *self, const gchar *name)
{
    g_autofree gchar *secrets = clawt_config_get_path_value(self->config, "secrets.dir");
    g_autofree gchar *file = g_strdup_printf("%s.token", name);

    return secrets != NULL ? g_build_filename(secrets, "office", "peers", file, NULL) : NULL;
}

/* A peer's name: what it is called on screen, so a short plain word. */
static gboolean
name_is_valid(const gchar *name)
{
    const gchar *p;

    if (name == NULL || *name == '\0' || strlen(name) > 32)
        return FALSE;
    for (p = name; *p != '\0'; p++)
        if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_')
            return FALSE;
    return TRUE;
}

/*
 * `NAME=WHERE`: a path (it starts with /) is a socket on this machine,
 * anything else host:port.  FALSE for an entry that is neither, which
 * the caller reports rather than dialling.
 */
static gboolean
parse_entry(const gchar *entry, gchar **out_name, gchar **out_target)
{
    const gchar *equals = entry != NULL ? strchr(entry, '=') : NULL;
    g_autofree gchar *name = NULL;

    if (equals == NULL || equals[1] == '\0')
        return FALSE;
    name = g_strndup(entry, (gsize)(equals - entry));
    g_strstrip(name);
    if (!name_is_valid(name))
        return FALSE;
    *out_name = g_steal_pointer(&name);
    *out_target = g_strstrip(g_strdup(equals + 1));
    return TRUE;
}

/* host:port, the port a real one */
static gboolean
split_host_port(const gchar *target, gchar **out_host, guint16 *out_port)
{
    const gchar *colon = strrchr(target, ':');
    guint64 port;
    gchar *end = NULL;

    if (colon == NULL || colon == target)
        return FALSE;
    port = g_ascii_strtoull(colon + 1, &end, 10);
    if (end == NULL || *end != '\0' || port == 0 || port > 65535)
        return FALSE;
    *out_host = g_strndup(target, (gsize)(colon - target));
    *out_port = (guint16)port;
    return TRUE;
}

static void
peer_set_error(Peer *peer, const gchar *message)
{
    /* said once per change, not once a poll */
    if (g_strcmp0(peer->error, message) != 0)
        g_message("office: the peer fleet '%s' (%s): %s", peer->name, peer->target, message);
    g_free(peer->error);
    peer->error = g_strdup(message);
}

/* ── An answer, made safe ───────────────────────────────────────── */

/* A string from a peer, or NULL when it is too long to be anything honest. */
static const gchar *
short_string(JsonObject *object, const gchar *member)
{
    JsonNode *node = json_object_get_member(object, member);
    const gchar *text;

    if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_STRING)
        return NULL;
    text = json_node_get_string(node);
    return text != NULL && g_utf8_validate(text, -1, NULL) &&
           g_utf8_strlen(text, -1) <= PEER_MAX_TEXT ? text : NULL;
}

/* an id as a fleet spells its own: letters, digits, - _ . and the : of grok:scout */
static gboolean
id_is_plain(const gchar *id)
{
    const gchar *p;

    if (id == NULL || *id == '\0')
        return FALSE;
    for (p = id; *p != '\0'; p++)
        if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_' && *p != '.' && *p != ':')
            return FALSE;
    return TRUE;
}

JsonArray *
clawt_daemon_office_peer_sanitize(const gchar  *peer_name,
                                  JsonObject   *snapshot,
                                  const gchar  *our_fleet,
                                  gchar       **out_fleet,
                                  GError      **error)
{
    JsonArray *given;
    JsonArray *out;
    const gchar *fleet;
    guint i;

    fleet = snapshot != NULL ? short_string(snapshot, "fleet") : NULL;
    given = snapshot != NULL && json_object_has_member(snapshot, "participants") &&
            JSON_NODE_HOLDS_ARRAY(json_object_get_member(snapshot, "participants"))
        ? json_object_get_array_member(snapshot, "participants") : NULL;
    if (fleet == NULL || given == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                            "its answer is not an office snapshot");
        return NULL;
    }
    /* a peer that is this fleet would draw every person twice, as their own visitor */
    if (g_strcmp0(fleet, our_fleet) == 0) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "it is this fleet: office.peers names this daemon");
        return NULL;
    }

    out = json_array_new();
    for (i = 0; i < json_array_get_length(given) && json_array_get_length(out) <
         PEER_MAX_PARTICIPANTS; i++) {
        JsonNode *node = json_array_get_element(given, i);
        JsonObject *them;
        JsonObject *visitor;
        const gchar *id;
        const gchar *name;
        const gchar *activity;
        g_autofree gchar *ours = NULL;

        if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node))
            continue;
        them = json_node_get_object(node);
        /* only their own people: a peer passing on a third fleet's is not believed */
        if (g_strcmp0(short_string(them, "fleet"), fleet) != 0)
            continue;
        id = short_string(them, "id");
        if (!id_is_plain(id))
            continue;
        name = short_string(them, "name");
        activity = short_string(them, "activity");
        /* an activity this clawtilla does not know is drawn as idle, not trusted */
        {
            ClawtOfficeActivity known;

            if (activity == NULL || !clawt_office_activity_from_nick(activity, &known))
                activity = "idle";
        }
        if (g_strcmp0(activity, "offline") == 0)
            continue;

        ours = g_strdup_printf("%s:%s", peer_name, id);
        visitor = json_object_new();
        json_object_set_string_member(visitor, "fleet", fleet);
        json_object_set_string_member(visitor, "kind", "external");
        json_object_set_string_member(visitor, "id", ours);
        /* the plain name: renderers add the fleet on hover, where there is room */
        json_object_set_string_member(visitor, "name", name != NULL ? name : id);
        json_object_set_string_member(visitor, "activity", activity);
        /* not "peer": in a snapshot that is who somebody is on a call with */
        json_object_set_string_member(visitor, "visiting_from", peer_name);
        {
            const gchar *color = short_string(them, "color");

            if (color != NULL && clawt_office_character_background_is_valid(color) &&
                color[0] == '#')
                json_object_set_string_member(visitor, "color", color);
        }
        json_object_set_int_member(visitor, "since",
                                   json_object_get_int_member_with_default(them, "since", 0));
        json_array_add_object_element(out, visitor);
    }
    if (out_fleet != NULL)
        *out_fleet = g_strdup(fleet);
    return out;
}

/* ── Asking ─────────────────────────────────────────────────────── */

static void
on_snapshot(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Peer *peer = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) reply = clawt_client_request_finish(CLAWT_CLIENT(source), result,
                                                            &error);
    g_autofree gchar *fleet = NULL;
    JsonArray *visitors;

    peer->in_flight = FALSE;
    if (reply == NULL || !JSON_NODE_HOLDS_OBJECT(reply)) {
        peer_set_error(peer, error != NULL ? error->message : "it answered nothing");
        return;
    }
    visitors = clawt_daemon_office_peer_sanitize(peer->name, json_node_get_object(reply),
                                                 clawt_daemon_fleet_id(peer->daemon),
                                                 &fleet, &error);
    if (visitors == NULL) {
        peer_set_error(peer, error->message);
        return;
    }
    g_clear_pointer(&peer->participants, json_array_unref);
    peer->participants = visitors;
    g_free(peer->fleet);
    peer->fleet = g_steal_pointer(&fleet);
    peer->fetched_at = g_get_real_time() / G_USEC_PER_SEC;
    g_clear_pointer(&peer->error, g_free);
}

static void
ask(Peer *peer)
{
    if (peer->in_flight || peer->client == NULL || !clawt_client_is_connected(peer->client))
        return;
    peer->in_flight = TRUE;
    clawt_client_request_async(peer->client, "office.snapshot", NULL, NULL, on_snapshot, peer);
}

static void
on_connected(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Peer *peer = user_data;
    g_autoptr(GError) error = NULL;

    peer->connecting = FALSE;
    if (!clawt_client_connect_finish(CLAWT_CLIENT(source), result, &error)) {
        peer_set_error(peer, error->message);
        /* dialled fresh next poll: a dead client is not kept */
        g_clear_object(&peer->client);
        return;
    }
    ask(peer);
}

static void
dial(Peer *peer)
{
    ClawtDaemon *self = peer->daemon;

    if (peer->target[0] == '/') {
        peer->client = clawt_client_new(peer->target);
    } else {
        g_autofree gchar *host = NULL;
        g_autofree gchar *path = token_path(self, peer->name);
        g_autofree gchar *token = NULL;
        guint16 port = 0;

        if (!split_host_port(peer->target, &host, &port)) {
            peer_set_error(peer, "not a socket path or host:port");
            return;
        }
        if (path == NULL || !g_file_get_contents(path, &token, NULL, NULL)) {
            peer_set_error(peer, "not paired yet: clawtilla office peer-pair NAME CODE");
            return;
        }
        g_strstrip(token);
        peer->client = clawt_client_new_tcp(host, port, token);
        memset(token, 0, strlen(token));
    }
    peer->connecting = TRUE;
    clawt_client_connect_async(peer->client, NULL, on_connected, peer);
}

static gboolean
on_poll(gpointer user_data)
{
    ClawtDaemon *self = user_data;
    guint i;

    if (self->main_context != NULL)
        g_main_context_push_thread_default(self->main_context);
    for (i = 0; self->office_peers != NULL && i < self->office_peers->len; i++) {
        Peer *peer = g_ptr_array_index(self->office_peers, i);

        if (peer->connecting)
            continue;
        if (peer->client == NULL || !clawt_client_is_connected(peer->client)) {
            g_clear_object(&peer->client);
            dial(peer);
        } else {
            ask(peer);
        }
    }
    if (self->main_context != NULL)
        g_main_context_pop_thread_default(self->main_context);
    return G_SOURCE_CONTINUE;
}

void
clawt_daemon_office_peers_touch(ClawtDaemon *self)
{
    g_auto(GStrv) entries = NULL;
    guint i;
    gint64 every;

    if (self->office_peers != NULL)
        return;
    self->office_peers = g_ptr_array_new_with_free_func((GDestroyNotify)peer_free);
    entries = clawt_config_get_string_list(self->config, "office.peers");
    for (i = 0; entries != NULL && entries[i] != NULL; i++) {
        Peer *peer;
        gchar *name = NULL;
        gchar *target = NULL;

        if (!parse_entry(entries[i], &name, &target)) {
            g_warning("office.peers: '%s' is not NAME=SOCKET or NAME=HOST:PORT", entries[i]);
            continue;
        }
        peer = g_new0(Peer, 1);
        peer->daemon = self;
        peer->name = name;
        peer->target = target;
        g_ptr_array_add(self->office_peers, peer);
    }
    if (self->office_peers->len == 0)
        return;

    every = MAX(1, clawt_config_get_int(self->config, "office.peer_poll_seconds"));
    /* on the daemon's own context: the timer and the client's sources both */
    if (self->main_context != NULL)
        g_main_context_push_thread_default(self->main_context);
    self->office_peer_tick = clawt_timeout_add_seconds((guint)every, on_poll, self);
    if (self->main_context != NULL)
        g_main_context_pop_thread_default(self->main_context);
    on_poll(self);
}

void
clawt_daemon_office_peers_merge(ClawtDaemon *self, JsonArray *participants, gint64 now)
{
    gint64 every = MAX(1, clawt_config_get_int(self->config, "office.peer_poll_seconds"));
    g_autoptr(GHashTable) linked = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                         g_free, NULL);
    guint i;
    guint j;

    /*
     * Linked fleets first (docs/federation.org): their people come from
     * the link's roster.  A fleet that is both linked and still named in
     * office.peers is drawn once, from the link, which is the one that
     * respects what that fleet chose to show us.
     */
    clawt_daemon_federation_office_visitors(self, participants, linked);

    for (i = 0; self->office_peers != NULL && i < self->office_peers->len; i++) {
        Peer *peer = g_ptr_array_index(self->office_peers, i);

        /* a peer that stopped answering is drawn empty, not frozen as it was */
        if (peer->participants == NULL || now - peer->fetched_at > PEER_STALE_POLLS * every)
            continue;
        if (peer->fleet != NULL && g_hash_table_contains(linked, peer->fleet))
            continue;
        for (j = 0; j < json_array_get_length(peer->participants); j++)
            json_array_add_element(participants,
                                   json_node_copy(json_array_get_element(peer->participants,
                                                                         j)));
    }
}

JsonNode *
clawt_daemon_office_peers_status(ClawtDaemon *self)
{
    g_autoptr(JsonBuilder) b = json_builder_new();
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    guint i;

    clawt_daemon_office_peers_touch(self);
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "peers");
    json_builder_begin_array(b);
    for (i = 0; i < self->office_peers->len; i++) {
        Peer *peer = g_ptr_array_index(self->office_peers, i);

        json_builder_begin_object(b);
        json_builder_set_member_name(b, "name");
        json_builder_add_string_value(b, peer->name);
        json_builder_set_member_name(b, "where");
        json_builder_add_string_value(b, peer->target);
        json_builder_set_member_name(b, "state");
        json_builder_add_string_value(b,
            peer->error != NULL ? "error"
            : peer->fetched_at == 0 ? "asking"
            : now - peer->fetched_at > PEER_STALE_POLLS *
                  MAX(1, clawt_config_get_int(self->config, "office.peer_poll_seconds"))
            ? "quiet" : "ok");
        if (peer->error != NULL) {
            json_builder_set_member_name(b, "error");
            json_builder_add_string_value(b, peer->error);
        }
        if (peer->fleet != NULL) {
            json_builder_set_member_name(b, "fleet");
            json_builder_add_string_value(b, peer->fleet);
        }
        json_builder_set_member_name(b, "visitors");
        json_builder_add_int_value(b, peer->participants != NULL
                                   ? json_array_get_length(peer->participants) : 0);
        if (peer->fetched_at > 0) {
            json_builder_set_member_name(b, "read_at");
            json_builder_add_int_value(b, peer->fetched_at);
        }
        json_builder_end_object(b);
    }
    json_builder_end_array(b);
    json_builder_end_object(b);
    return json_builder_get_root(b);
}

void
clawt_daemon_office_peers_stop(ClawtDaemon *self)
{
    if (self->office_peer_tick != NULL) {
        g_source_destroy(self->office_peer_tick);
        g_clear_pointer(&self->office_peer_tick, g_source_unref);
    }
    g_clear_pointer(&self->office_peers, g_ptr_array_unref);
}

/* ── Pairing ────────────────────────────────────────────────────── */

typedef struct {
    ClawtDaemon     *daemon;
    ClawtIpcPending *pending;
    ClawtClient     *client;
    gchar           *name;
} Pairing;

static void
pairing_free(Pairing *p)
{
    if (p->client != NULL)
        clawt_client_disconnect(p->client);
    g_clear_object(&p->client);
    g_object_unref(p->daemon);
    g_free(p->name);
    g_free(p);
}

static void
on_paired(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Pairing *p = user_data;
    g_autoptr(GError) error = NULL;
    JsonNode *request = clawt_ipc_pending_get_request(p->pending);
    g_autofree gchar *path = token_path(p->daemon, p->name);
    g_autofree gchar *dir = NULL;
    const gchar *token;

    if (!clawt_client_connect_finish(CLAWT_CLIENT(source), result, &error)) {
        clawt_ipc_pending_respond(p->pending, clawt_ipc_error_new(request, CLAWT_ERROR_AUTH,
                                                                  error->message));
        pairing_free(p);
        return;
    }
    token = clawt_client_get_token(p->client);
    dir = path != NULL ? g_path_get_dirname(path) : NULL;
    if (token == NULL || dir == NULL || g_mkdir_with_parents(dir, 0700) != 0 ||
        !g_file_set_contents_full(path, token, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600,
                                  &error)) {
        clawt_ipc_pending_respond(p->pending, clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
            error != NULL ? error->message : "the peer gave no token"));
        pairing_free(p);
        return;
    }
    {
        g_autoptr(JsonBuilder) b = json_builder_new();

        /* the file, never the token: it crosses IPC this once, to nobody */
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "token_file");
        json_builder_add_string_value(b, path);
        json_builder_end_object(b);
        clawt_ipc_pending_respond(p->pending,
                                  clawt_ipc_response_new(request, json_builder_get_root(b)));
    }
    /* dialled again with the token next poll */
    clawt_daemon_office_peers_stop(p->daemon);
    pairing_free(p);
}

JsonNode *
clawt_daemon_office_peer_pair(ClawtDaemon *self, JsonNode *request, const gchar *name,
                              const gchar *code)
{
    g_auto(GStrv) entries = clawt_config_get_string_list(self->config, "office.peers");
    g_autofree gchar *target = NULL;
    g_autofree gchar *host = NULL;
    guint16 port = 0;
    Pairing *p;
    guint i;

    for (i = 0; entries != NULL && entries[i] != NULL && target == NULL; i++) {
        g_autofree gchar *n = NULL;
        g_autofree gchar *t = NULL;

        if (parse_entry(entries[i], &n, &t) && g_strcmp0(n, name) == 0)
            target = g_steal_pointer(&t);
    }
    if (target == NULL)
        return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                   "office.peers names no fleet by that name");
    if (target[0] == '/')
        return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                   "a peer on this machine's socket needs no pairing");
    if (!split_host_port(target, &host, &port))
        return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                   "the peer's address is not host:port");
    if (code == NULL || *code == '\0')
        return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                   "a pairing code is needed: clawtilla device pair, there");

    p = g_new0(Pairing, 1);
    p->pending = clawt_ipc_server_defer(self->ipc_server, request);
    if (p->pending == NULL) {
        g_free(p);
        return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                   "this request cannot be answered later");
    }
    p->daemon = g_object_ref(self);
    p->name = g_strdup(name);
    p->client = clawt_client_new_tcp(host, port, NULL);
    {
        g_autofree gchar *me = g_strdup_printf("fleet %.8s (office)", clawt_daemon_fleet_id(self));

        clawt_client_set_pairing_code(p->client, code, me);
    }
    if (self->main_context != NULL)
        g_main_context_push_thread_default(self->main_context);
    clawt_client_connect_async(p->client, NULL, on_paired, p);
    if (self->main_context != NULL)
        g_main_context_pop_thread_default(self->main_context);
    return NULL;
}

/*
 * office.peers as federation sees it, for `federation migrate-office`
 * (0.71.0): each entry's name, where it is, the fleet id it last gave
 * (when it has answered) and its stored token, read from the file -- the
 * one thing that cannot be got again without pairing.
 */
JsonArray *
clawt_daemon_office_peers_entries(ClawtDaemon *self)
{
    g_auto(GStrv) entries = clawt_config_get_string_list(self->config, "office.peers");
    JsonArray *out = json_array_new();
    guint i;
    guint j;

    for (i = 0; entries != NULL && entries[i] != NULL; i++) {
        g_autofree gchar *name = NULL;
        g_autofree gchar *target = NULL;
        g_autofree gchar *path = NULL;
        g_autofree gchar *token = NULL;
        JsonObject *e;

        if (!parse_entry(entries[i], &name, &target))
            continue;
        e = json_object_new();
        json_object_set_string_member(e, "name", name);
        json_object_set_string_member(e, "where", target);
        path = token_path(self, name);
        if (target[0] != '/' && path != NULL && g_file_get_contents(path, &token, NULL, NULL)) {
            g_strstrip(token);
            json_object_set_string_member(e, "token", token);
        }
        for (j = 0; self->office_peers != NULL && j < self->office_peers->len; j++) {
            Peer *peer = g_ptr_array_index(self->office_peers, j);
            if (g_strcmp0(peer->name, name) == 0 && peer->fleet != NULL)
                json_object_set_string_member(e, "fleet", peer->fleet);
        }
        json_array_add_object_element(out, e);
    }
    return out;
}
