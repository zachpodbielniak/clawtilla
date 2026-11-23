/*
 * daemon-federation.c - This fleet, linked with others
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The daemon's half of federation (docs/federation.org):
 *
 *  - its identity: a fleet key under <secrets.dir>/federation, whose
 *    first fingerprint is the fleet id for good;
 *  - its doors: the HTTP listener (federation.listen, a unix socket by
 *    default for cloudflared) and, with federation.ipc, its own IPC
 *    port -- the simple daemon connection;
 *  - one verification path for everything that arrives, whichever door
 *    it came through: a known fleet, a key it still holds, a stamp in
 *    the window, a nonce never seen, addressed to us -- and only then is
 *    the body read;
 *  - a link to every peer that has an endpoint, pinged on a timer, its
 *    health published as it changes;
 *  - invites, accepts, links between fleets one person owns, revocation
 *    and key rotation;
 *  - an audit of every request in either direction, refusals included;
 *  - the operator's `federation.*` requests.
 *
 * Nothing here runs unless federation.enabled, and nothing waits on the
 * network from an IPC handler or from daemon start: links are dialled
 * from a timer, and the requests that need an answer from another fleet
 * (accept, link, ping) are deferred and answered when it comes.
 */

#include "clawtilla.h"
#include "clawt-daemon-private.h"

#include <errno.h>
#include <string.h>
#include <glib/gstdio.h>

#define AUDIT_RING (500)
#define FED_PROTOCOL_VERSION (1)

typedef struct {
    ClawtDaemon     *daemon;        /* not owned */
    gchar           *alias;
    ClawtFedLink    *link;          /* nullable: a peer with no endpoint */
    ClawtFedHealth   health;
    gint64           last_ok;       /* unix seconds of the last good answer */
    gint64           last_seen;     /* ... or request from it */
    gint64           next_try;      /* unix seconds; 0 = now */
    guint            backoff;       /* seconds */
    guint            misses;
    gboolean         in_flight;
    gint64           latency_ms;
    gchar           *error;
} PeerState;

static void
peer_state_free(
    PeerState *ps
){
    if (ps->link != NULL) {
        g_signal_handlers_disconnect_by_data(ps->link, ps);
        clawt_fed_link_close(ps->link);
        g_object_unref(ps->link);
    }
    g_free(ps->alias);
    g_free(ps->error);
    g_free(ps);
}

static gint64
now_s(void)
{
    return g_get_real_time() / G_USEC_PER_SEC;
}

static gboolean
fed_on(
    ClawtDaemon *self
){
    return self->fed_registry != NULL && self->fed_key != NULL;
}

/* ---- identity ---------------------------------------------------------- */

static gchar *
secrets_path(
    ClawtDaemon *self,
    const gchar *name
){
    g_autofree gchar *secrets = clawt_config_get_path_value(self->config, "secrets.dir");

    if (secrets == NULL)
        return NULL;
    return g_build_filename(secrets, "federation", name, NULL);
}

static gchar *
state_path(
    ClawtDaemon *self,
    const gchar *name
){
    return g_build_filename(self->state_dir, "federation", name, NULL);
}

/*
 * The fleet id and current key index, kept beside the key: the id is the
 * fingerprint of the *first* key and must outlive a rotation.
 */
static void
load_identity(
    ClawtDaemon *self
){
    g_autofree gchar *path = secrets_path(self, "fleet.meta");
    g_autofree gchar *text = NULL;
    g_auto(GStrv) lines = NULL;
    guint i;

    g_clear_pointer(&self->fed_fleet_id, g_free);
    self->fed_key_index = 0;
    if (path != NULL && g_file_get_contents(path, &text, NULL, NULL)) {
        lines = g_strsplit(text, "\n", -1);
        for (i = 0; lines[i] != NULL; i++) {
            if (g_str_has_prefix(lines[i], "fleet_id="))
                self->fed_fleet_id = g_strstrip(g_strdup(lines[i] + 9));
            else if (g_str_has_prefix(lines[i], "key_index="))
                self->fed_key_index = (guint)g_ascii_strtoull(lines[i] + 10, NULL, 10);
        }
    }
    if (self->fed_fleet_id == NULL || strlen(self->fed_fleet_id) != CLAWT_FLEET_FINGERPRINT_LENGTH) {
        g_free(self->fed_fleet_id);
        self->fed_fleet_id = clawt_fleet_key_fleet_id(self->fed_key);
        self->fed_key_index = 0;
    }
}

static gboolean
save_identity(
    ClawtDaemon  *self,
    GError      **error
){
    g_autofree gchar *path = secrets_path(self, "fleet.meta");
    g_autofree gchar *text = NULL;

    text = g_strdup_printf("fleet_id=%s\nkey_index=%u\n", self->fed_fleet_id,
                           self->fed_key_index);
    return clawt_write_file_atomic(path, text, -1, 0600, FALSE, error);
}

static const gchar *
fleet_name(
    ClawtDaemon *self
){
    const gchar *name = clawt_config_get_string(self->config, "federation.name");

    return name != NULL && *name != '\0' ? name : g_get_host_name();
}

/* ---- audit ------------------------------------------------------------- */

static void
audit(
    ClawtDaemon *self,
    const gchar *direction,
    const gchar *peer,
    const gchar *kind,
    const gchar *outcome,
    const gchar *detail
){
    g_autoptr(JsonBuilder) b = json_builder_new();
    g_autoptr(JsonGenerator) g = json_generator_new();
    g_autoptr(JsonNode) root = NULL;
    g_autofree gchar *line = NULL;
    g_autofree gchar *path = NULL;
    g_autoptr(ClawtEvent) event = NULL;
    FILE *f;

    /*
     * A link's keep-alive is not activity: a successful hello or ping
     * every few seconds per peer would bury everything else in the
     * audit and redraw every client's Federation page each time.  Their
     * failures, and every other request, are recorded.
     */
    if (g_strcmp0(outcome, "ok") == 0
        && (g_strcmp0(kind, "hello") == 0 || g_strcmp0(kind, "ping") == 0))
        return;

    json_builder_begin_object(b);
    json_builder_set_member_name(b, "time");
    json_builder_add_int_value(b, now_s());
    json_builder_set_member_name(b, "direction");
    json_builder_add_string_value(b, direction);
    json_builder_set_member_name(b, "peer");
    json_builder_add_string_value(b, peer != NULL ? peer : "");
    json_builder_set_member_name(b, "kind");
    json_builder_add_string_value(b, kind != NULL ? kind : "");
    json_builder_set_member_name(b, "outcome");
    json_builder_add_string_value(b, outcome);
    json_builder_set_member_name(b, "detail");
    json_builder_add_string_value(b, detail != NULL ? detail : "");
    json_builder_end_object(b);
    root = json_builder_get_root(b);

    g_queue_push_tail(self->fed_audit, json_object_ref(json_node_get_object(root)));
    while (g_queue_get_length(self->fed_audit) > AUDIT_RING)
        json_object_unref(g_queue_pop_head(self->fed_audit));

    json_generator_set_root(g, root);
    line = json_generator_to_data(g, NULL);
    path = state_path(self, "audit.ndjson");
    f = fopen(path, "a");
    if (f != NULL) {
        fprintf(f, "%s\n", line);
        fclose(f);
    }

    if (self->bus != NULL) {
        event = clawt_event_new("federation.audit", peer);
        clawt_event_set_detail(event, "direction", direction);
        clawt_event_set_detail(event, "kind", kind != NULL ? kind : "");
        clawt_event_set_detail(event, "outcome", outcome);
        clawt_event_bus_publish(self->bus, event);
    }
}

/* Keeps the last federation.audit_retention_days of the file. */
static void
audit_prune(
    ClawtDaemon *self
){
    g_autofree gchar *path = state_path(self, "audit.ndjson");
    g_autofree gchar *text = NULL;
    g_auto(GStrv) lines = NULL;
    g_autoptr(GString) kept = g_string_new(NULL);
    gint64 cutoff;
    guint i;
    guint dropped = 0;

    cutoff = now_s() - clawt_config_get_int(self->config, "federation.audit_retention_days") * 86400;
    if (!g_file_get_contents(path, &text, NULL, NULL))
        return;
    lines = g_strsplit(text, "\n", -1);
    for (i = 0; lines[i] != NULL; i++) {
        const gchar *t = strstr(lines[i], "\"time\":");
        if (*lines[i] == '\0')
            continue;
        if (t != NULL && g_ascii_strtoll(t + 7, NULL, 10) < cutoff) {
            dropped++;
            continue;
        }
        g_string_append(kept, lines[i]);
        g_string_append_c(kept, '\n');
    }
    if (dropped > 0)
        clawt_write_file_atomic(path, kept->str, kept->len, 0600, FALSE, NULL);
}

static void
publish_peer(
    ClawtDaemon *self,
    const gchar *alias,
    const gchar *change
){
    g_autoptr(ClawtEvent) event = NULL;

    if (self->bus == NULL)
        return;
    event = clawt_event_new("federation.peer", alias);
    clawt_event_set_detail(event, "alias", alias);
    clawt_event_set_detail(event, "change", change);
    clawt_event_bus_publish(self->bus, event);
}

static void
save_registry(
    ClawtDaemon *self
){
    g_autoptr(GError) error = NULL;

    if (!clawt_fed_registry_save(self->fed_registry, &error))
        g_warning("federation: the peer list was not saved: %s", error->message);
}

/* ---- JSON helpers ------------------------------------------------------ */

static gchar *
to_text(
    JsonNode *node
){
    g_autoptr(JsonGenerator) g = json_generator_new();

    json_generator_set_root(g, node);
    return json_generator_to_data(g, NULL);
}

static gchar *
object_text(
    JsonObject *object
){
    g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);

    json_node_set_object(node, object);
    return to_text(node);
}

static JsonObject *
parse_object(
    const gchar *text
){
    g_autoptr(JsonParser) parser = json_parser_new();
    JsonNode *root;

    if (text == NULL || !json_parser_load_from_data(parser, text, -1, NULL))
        return NULL;
    root = json_parser_get_root(parser);
    if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root) || json_node_get_object(root) == NULL)
        return NULL;
    return json_object_ref(json_node_get_object(root));
}

static gchar *
error_body(
    const gchar *message
){
    g_autoptr(JsonObject) o = json_object_new();

    json_object_set_string_member(o, "error", message);
    return object_text(o);
}

static const gchar *
obj_str(
    JsonObject  *o,
    const gchar *name
){
    JsonNode *n = o != NULL ? json_object_get_member(o, name) : NULL;

    if (n == NULL || !JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_STRING)
        return NULL;
    return json_node_get_string(n);
}

/* A short, single-line, printable copy of something another fleet said. */
static gchar *
bounded(
    const gchar *text,
    glong        max
){
    g_autofree gchar *valid = NULL;
    GString *out;
    const gchar *p;
    glong n = 0;

    if (text == NULL)
        return NULL;
    valid = g_utf8_make_valid(text, -1);
    out = g_string_new(NULL);
    for (p = valid; *p != '\0' && n < max; p = g_utf8_next_char(p), n++) {
        gunichar c = g_utf8_get_char(p);
        if (g_unichar_iscntrl(c))
            g_string_append_c(out, ' ');
        else
            g_string_append_unichar(out, c);
    }
    return g_string_free(out, FALSE);
}

/* ---- what we tell another fleet about ourselves ----------------------- */

static gchar *
default_endpoint(
    ClawtDaemon *self
){
    const gchar *public_url = clawt_config_get_string(self->config, "federation.public_url");
    g_auto(GStrv) bound = NULL;
    guint i;

    if (public_url != NULL && *public_url != '\0')
        return g_strdup(public_url);

    /* The simple daemon connection on a tailnet address, when there is one. */
    if (clawt_config_get_boolean(self->config, "federation.ipc") && self->ipc_server != NULL) {
        bound = clawt_ipc_server_list_bound(self->ipc_server);
        for (i = 0; bound != NULL && bound[i] != NULL; i++) {
            g_autofree gchar *endpoint = g_strdup_printf("clawtilla://%s", bound[i]);
            g_autofree gchar *host = NULL;
            ClawtFedPath path;

            if (!clawt_fed_endpoint_parse(endpoint, NULL, &host, NULL, NULL, NULL))
                continue;
            path = clawt_fed_host_path(host);
            if (path == CLAWT_FED_PATH_TAILNET)
                return g_steal_pointer(&endpoint);
        }
    }
    /* Then the HTTP listener, if it is bound somewhere another machine can reach. */
    if (self->fed_server != NULL) {
        g_auto(GStrv) listening = clawt_fed_server_list_bound(self->fed_server);
        for (i = 0; listening[i] != NULL; i++) {
            g_autofree gchar *endpoint = NULL;
            g_autofree gchar *host = NULL;
            ClawtFedPath path;

            if (g_str_has_prefix(listening[i], "unix:"))
                continue;
            endpoint = g_strdup_printf("http://%s", listening[i]);
            if (!clawt_fed_endpoint_parse(endpoint, NULL, &host, NULL, NULL, NULL))
                continue;
            path = clawt_fed_host_path(host);
            if (path == CLAWT_FED_PATH_TAILNET || path == CLAWT_FED_PATH_LAN)
                return g_steal_pointer(&endpoint);
        }
    }
    return NULL;
}

/* The pin of this daemon's IPC certificate, when it has one. */
static gchar *
our_tls_pin(
    ClawtDaemon *self
){
    const gchar *cert = clawt_config_get_string(self->config, "daemon.tls_cert");
    const gchar *key = clawt_config_get_string(self->config, "daemon.tls_key");
    g_autoptr(GTlsCertificate) loaded = NULL;

    if (cert == NULL || *cert == '\0' || key == NULL || *key == '\0')
        return NULL;
    loaded = g_tls_certificate_new_from_files(cert, key, NULL);
    return loaded != NULL ? clawt_tls_certificate_pin(loaded) : NULL;
}

static JsonObject *
self_description(
    ClawtDaemon *self
){
    JsonObject *o = json_object_new();
    g_autofree gchar *key = clawt_fleet_key_public_text(self->fed_key);
    g_autofree gchar *seal = clawt_fleet_key_seal_public_text(self->fed_key);
    const gchar *owner = clawt_config_get_string(self->config, "federation.owner");
    g_autofree gchar *endpoint = default_endpoint(self);

    json_object_set_string_member(o, "protocol", CLAWT_FED_PROTOCOL);
    json_object_set_string_member(o, "fleet", self->fed_fleet_id);
    json_object_set_string_member(o, "key", key);
    json_object_set_int_member(o, "key_index", self->fed_key_index);
    json_object_set_string_member(o, "seal_key", seal);
    json_object_set_string_member(o, "name", fleet_name(self));
    if (owner != NULL && *owner != '\0')
        json_object_set_string_member(o, "owner", owner);
    if (endpoint != NULL)
        json_object_set_string_member(o, "endpoint", endpoint);
    json_object_set_string_member(o, "version", CLAWT_VERSION_STRING);
    return o;
}

static const gchar *
inbox_agent(
    ClawtDaemon *self
){
    const gchar *configured = clawt_config_get_string(self->config, "federation.inbox_agent");
    ClawtAgent *chief;

    if (configured != NULL && *configured != '\0')
        return configured;
    chief = self->agents != NULL ? clawt_agent_manager_get_chief_of_staff(self->agents) : NULL;
    return chief != NULL ? clawt_agent_get_id(chief) : NULL;
}

/* The grant we give @peer, and why each field is what it is. */
static ClawtFedGrant *
grant_for(
    ClawtDaemon   *self,
    ClawtFedPeer  *peer,
    ClawtFedHealth health,
    GHashTable   **out_explain,
    GPtrArray    **out_problems
){
    g_auto(GStrv) rules = clawt_config_get_string_list(self->config, "federation.rules");
    g_autoptr(JsonObject) facts = clawt_fed_peer_selector_json(peer, health);

    return clawt_fed_grant_resolve(peer->tier, rules, facts, peer->overrides,
                                   out_explain, out_problems);
}

static PeerState *
state_of(
    ClawtDaemon *self,
    const gchar *alias
){
    return self->fed_links != NULL ? g_hash_table_lookup(self->fed_links, alias) : NULL;
}

static ClawtFedHealth
health_of(
    ClawtDaemon  *self,
    ClawtFedPeer *peer
){
    PeerState *ps;
    gint64 window;

    if (g_strcmp0(peer->status, "revoked") == 0)
        return CLAWT_FED_HEALTH_REVOKED;
    if (g_strcmp0(peer->status, "pending") == 0)
        return CLAWT_FED_HEALTH_PENDING;
    ps = state_of(self, peer->alias);
    if (ps == NULL)
        return CLAWT_FED_HEALTH_PENDING;
    if (ps->link != NULL)
        return ps->health;
    /* A peer that only dials us is as live as its last request. */
    window = 3 * MAX(clawt_config_get_int(self->config, "federation.ping_seconds"), 5);
    if (ps->last_seen == 0)
        return CLAWT_FED_HEALTH_PENDING;
    return now_s() - ps->last_seen <= window ? CLAWT_FED_HEALTH_LIVE : CLAWT_FED_HEALTH_OFFLINE;
}

static void
set_health(
    PeerState      *ps,
    ClawtFedHealth  health,
    const gchar    *error
){
    gboolean changed = ps->health != health || g_strcmp0(ps->error, error) != 0;

    ps->health = health;
    g_free(ps->error);
    ps->error = g_strdup(error);
    if (changed)
        publish_peer(ps->daemon, ps->alias, "health");
}

/* ---- answering another fleet ------------------------------------------ */

typedef struct {
    guint  status;
    gchar *body;
    gchar *peer_alias;   /* who it turned out to be, for the audit */
} Answer;

static void
answer_set(
    Answer      *a,
    guint        status,
    gchar       *body
){
    a->status = status;
    g_free(a->body);
    a->body = body;
}

static void
answer_error(
    Answer      *a,
    guint        status,
    const gchar *message
){
    answer_set(a, status, error_body(message));
}

/* An invite redeemed: the accepting fleet becomes a peer here. */
static void
receive_invite(
    ClawtDaemon   *self,
    ClawtFedStamp *stamp,
    const gchar   *body,
    const gchar   *binding,
    Answer        *answer
){
    g_autoptr(JsonObject) o = parse_object(body);
    g_autoptr(GError) error = NULL;
    g_autofree gchar *fingerprint = NULL;
    g_autofree gchar *proof = NULL;
    g_autofree gchar *alias = NULL;
    g_autofree gchar *name = NULL;
    g_autoptr(JsonObject) me = NULL;
    ClawtFedPendingInvite *invite;
    ClawtFedPeer *peer;
    ClawtFedPeer *existing;
    const gchar *key;
    const gchar *invite_id;
    guint8 raw[CLAWT_FLEET_KEY_SIZE];
    gint64 now = now_s();
    gboolean confirm;
    guint n;

    key = obj_str(o, "key");
    invite_id = obj_str(o, "invite_id");
    if (o == NULL || key == NULL || invite_id == NULL || !clawt_fed_decode_key(key, raw)) {
        answer_error(answer, 400, "an accept carries invite_id and key");
        return;
    }
    /* The key must be the one the sender's fleet id is the fingerprint of. */
    fingerprint = clawt_fleet_key_fingerprint(raw);
    if (g_strcmp0(fingerprint, stamp->from) != 0
        || !clawt_fed_stamp_check(stamp, raw, self->fed_fleet_id, body, binding, now,
                                  (guint)clawt_config_get_int(self->config, "federation.clock_skew_seconds"),
                                  &error)
        || !clawt_fed_replay_guard_admit(self->fed_guard, stamp->from, stamp->nonce,
                                         stamp->time, now)) {
        answer_error(answer, 401, "not signed by the fleet it claims to be");
        return;
    }

    invite = clawt_fed_registry_find_invite(self->fed_registry, invite_id);
    if (invite == NULL || invite->expires < now) {
        answer_error(answer, 403, "that invite was used, revoked or has expired");
        return;
    }
    /*
     * The proof covers the time the accepter made it, carried in the
     * signed body; it must be inside the window like the stamp is.
     */
    {
        JsonNode *t = json_object_get_member(o, "proof_time");
        gint64 proof_time = (t != NULL && JSON_NODE_HOLDS_VALUE(t)
                             && json_node_get_value_type(t) == G_TYPE_INT64)
                            ? json_node_get_int(t) : 0;
        gint64 skew = clawt_config_get_int(self->config, "federation.clock_skew_seconds");
        if (proof_time - now > skew || now - proof_time > skew) {
            answer_error(answer, 403, "that invite was used, revoked or has expired");
            return;
        }
        proof = clawt_fed_proof(invite->secret, key, self->fed_fleet_id, proof_time);
    }
    if (!clawt_secure_equals(proof, obj_str(o, "proof"))) {
        answer_error(answer, 403, "that invite was used, revoked or has expired");
        return;
    }

    existing = clawt_fed_registry_by_fleet(self->fed_registry, stamp->from);
    if (existing != NULL && g_strcmp0(existing->status, "revoked") != 0) {
        answer_error(answer, 409, "that fleet is already linked here");
        return;
    }
    if (existing != NULL)
        clawt_fed_registry_remove(self->fed_registry, existing->alias);

    /* An alias from its name, made unique. */
    name = bounded(obj_str(o, "name"), 64);
    alias = clawt_fed_alias_suggest(name);
    for (n = 2; clawt_fed_registry_get(self->fed_registry, alias) != NULL; n++) {
        g_autofree gchar *base = clawt_fed_alias_suggest(name);
        g_free(alias);
        alias = g_strdup_printf("%.28s-%u", base, n);
    }

    peer = clawt_fed_peer_new(alias, stamp->from, key);
    peer->label = g_strdup(name);
    peer->owner = bounded(obj_str(o, "owner"), 64);
    peer->version = bounded(obj_str(o, "version"), 32);
    peer->tier = invite->tier;
    if (obj_str(o, "seal_key") != NULL && clawt_fed_decode_key(obj_str(o, "seal_key"), raw))
        peer->seal_key = g_strdup(obj_str(o, "seal_key"));
    if (obj_str(o, "endpoint") != NULL && clawt_fed_endpoint_check(obj_str(o, "endpoint"), NULL))
        peer->endpoint = g_strdup(obj_str(o, "endpoint"));
    /* Read before the invite is spent: removing it frees it. */
    confirm = invite->confirm;
    if (confirm) {
        g_free(peer->status);
        peer->status = g_strdup("pending");
    }
    clawt_fed_registry_remove_invite(self->fed_registry, invite_id);
    invite = NULL;
    if (!clawt_fed_registry_add(self->fed_registry, peer, &error)) {
        answer_error(answer, 409, error->message);
        return;
    }
    save_registry(self);
    answer->peer_alias = g_strdup(alias);

    me = self_description(self);
    json_object_set_string_member(me, "status", confirm ? "pending" : "active");
    answer_set(answer, 200, object_text(me));
    clawt_daemon_federation_refresh_links(self);
    publish_peer(self, alias, "added");
}

/* A signed rotation: the old key vouches for the new one. */
static void
receive_rotate(
    ClawtDaemon  *self,
    ClawtFedPeer *peer,
    JsonObject   *o,
    Answer       *answer
){
    const gchar *key = obj_str(o, "key");
    const gchar *proof = obj_str(o, "proof");
    g_autofree gchar *text = NULL;
    guint8 raw[CLAWT_FLEET_KEY_SIZE];
    guint index;

    if (key == NULL || proof == NULL || !clawt_fed_decode_key(key, raw)) {
        answer_error(answer, 400, "a rotation carries key and proof");
        return;
    }
    text = g_strdup_printf("rotate|%s|%s", peer->fleet_id, key);
    if (!clawt_fleet_key_verify_text(raw, text, proof)) {
        answer_error(answer, 401, "the new key did not sign its rotation");
        return;
    }
    /* Already known (a retried notice): nothing to do. */
    for (index = 0; index < peer->keys->len; index++)
        if (g_strcmp0(g_ptr_array_index(peer->keys, index), key) == 0) {
            answer_set(answer, 200, g_strdup("{\"ok\":true}"));
            return;
        }
    clawt_fed_peer_rotate(peer, key, now_s());
    if (obj_str(o, "seal_key") != NULL && clawt_fed_decode_key(obj_str(o, "seal_key"), raw)) {
        g_free(peer->seal_key);
        peer->seal_key = g_strdup(obj_str(o, "seal_key"));
    }
    save_registry(self);
    clawt_daemon_federation_refresh_links(self);
    answer_set(answer, 200, g_strdup("{\"ok\":true}"));
}

static void
receive_hello(
    ClawtDaemon  *self,
    ClawtFedPeer *peer,
    JsonObject   *o,
    Answer       *answer
){
    g_autoptr(ClawtFedGrant) grant = NULL;
    g_autoptr(JsonObject) me = self_description(self);
    gboolean changed = FALSE;

    /* What it says about itself, bounded; never its keys or fleet id. */
    if (o != NULL) {
        g_autofree gchar *label = bounded(obj_str(o, "name"), 64);
        g_autofree gchar *owner = bounded(obj_str(o, "owner"), 64);
        g_autofree gchar *version = bounded(obj_str(o, "version"), 32);
        const gchar *endpoint = obj_str(o, "endpoint");

        if (label != NULL && g_strcmp0(label, peer->label) != 0) {
            g_free(peer->label);
            peer->label = g_steal_pointer(&label);
            changed = TRUE;
        }
        if (owner != NULL && g_strcmp0(owner, peer->owner) != 0) {
            g_free(peer->owner);
            peer->owner = g_steal_pointer(&owner);
            changed = TRUE;
        }
        if (version != NULL && g_strcmp0(version, peer->version) != 0) {
            g_free(peer->version);
            peer->version = g_steal_pointer(&version);
            changed = TRUE;
        }
        if (endpoint != NULL && g_strcmp0(endpoint, peer->endpoint) != 0
            && clawt_fed_endpoint_check(endpoint, NULL)) {
            g_free(peer->endpoint);
            peer->endpoint = g_strdup(endpoint);
            changed = TRUE;
        }
    }
    if (changed) {
        save_registry(self);
        clawt_daemon_federation_refresh_links(self);
        publish_peer(self, peer->alias, "changed");
    }

    grant = grant_for(self, peer, health_of(self, peer), NULL, NULL);
    json_object_set_object_member(me, "grant", clawt_fed_grant_to_json(grant));
    json_object_set_string_member(me, "tier", clawt_fed_tier_nick(peer->tier));
    json_object_set_string_member(me, "status", peer->status);
    answer_set(answer, 200, object_text(me));
}

/*
 * Everything that arrives, through any door.  @binding is what the
 * stamp must have been signed for on this door.
 */
static void
receive(
    ClawtDaemon   *self,
    ClawtFedStamp *stamp,
    const gchar   *body,
    const gchar   *binding,
    gboolean       invite_route,
    Answer        *answer
){
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonObject) o = NULL;
    g_autofree gchar *opened = NULL;
    ClawtFedPeer *peer;
    PeerState *ps;
    guint8 key[CLAWT_FLEET_KEY_SIZE];
    gint64 now = now_s();
    guint skew = (guint)clawt_config_get_int(self->config, "federation.clock_skew_seconds");

    if (stamp == NULL) {
        answer_error(answer, 401, "unsigned");
        return;
    }
    if (invite_route) {
        receive_invite(self, stamp, body, binding, answer);
        return;
    }

    /* Who: a fleet we know, with a key it still holds. */
    peer = clawt_fed_registry_by_fleet(self->fed_registry, stamp->from);
    if (peer == NULL || !clawt_fed_peer_public_key(peer, stamp->key_index, now, key)) {
        answer_error(answer, 401, "unknown fleet or key");
        return;
    }
    answer->peer_alias = g_strdup(peer->alias);
    if (!clawt_fed_stamp_check(stamp, key, self->fed_fleet_id, body, binding, now, skew, &error)) {
        g_debug("federation: refused a request from %s: %s", peer->alias, error->message);
        answer_error(answer, 401, "the signature did not check out");
        return;
    }
    if (!clawt_fed_replay_guard_admit(self->fed_guard, stamp->from, stamp->nonce, stamp->time, now)) {
        answer_error(answer, 401, "replayed");
        return;
    }
    if (g_strcmp0(peer->status, "revoked") == 0) {
        answer_error(answer, 403, "this link was revoked");
        return;
    }

    ps = state_of(self, peer->alias);
    if (ps != NULL)
        ps->last_seen = now;

    if (clawt_fed_body_is_sealed(body)) {
        guint8 seal[CLAWT_FLEET_KEY_SIZE];
        if (peer->seal_key == NULL || !clawt_fed_decode_key(peer->seal_key, seal)
            || (opened = clawt_fed_unseal(self->fed_key, seal, body, NULL)) == NULL) {
            answer_error(answer, 400, "the sealed body did not open");
            return;
        }
    } else {
        opened = g_strdup(body);
    }
    o = parse_object(opened);

    /* A fleet waiting for approval may only say hello and ping. */
    if (g_strcmp0(peer->status, "pending") == 0
        && g_strcmp0(stamp->kind, "hello") != 0 && g_strcmp0(stamp->kind, "ping") != 0) {
        answer_error(answer, 403, "this link is waiting for the other fleet's approval");
        return;
    }

    if (g_strcmp0(stamp->kind, "hello") == 0) {
        receive_hello(self, peer, o, answer);
    } else if (g_strcmp0(stamp->kind, "ping") == 0) {
        g_autoptr(JsonObject) pong = json_object_new();
        json_object_set_boolean_member(pong, "pong", TRUE);
        json_object_set_int_member(pong, "time", now);
        json_object_set_string_member(pong, "status", peer->status);
        answer_set(answer, 200, object_text(pong));
    } else if (g_strcmp0(stamp->kind, "revoke") == 0) {
        g_free(peer->status);
        peer->status = g_strdup("revoked");
        save_registry(self);
        clawt_daemon_federation_refresh_links(self);
        publish_peer(self, peer->alias, "revoked");
        answer_set(answer, 200, g_strdup("{\"ok\":true}"));
    } else if (g_strcmp0(stamp->kind, "rotate") == 0) {
        receive_rotate(self, peer, o, answer);
    } else {
        g_autofree gchar *message = g_strdup_printf(
            "this fleet (clawtilla %s) does not understand \"%s\" yet",
            CLAWT_VERSION_STRING, stamp->kind);
        answer_error(answer, 501, message);
    }
}

/*
 * The answer's stamp: from us, to whoever asked, bound to the nonce it
 * asked with.  Sealed when the request was.
 */
static ClawtFedStamp *
sign_answer(
    ClawtDaemon   *self,
    ClawtFedStamp *request,
    Answer        *answer
){
    g_autofree gchar *binding = NULL;
    ClawtFedPeer *peer;

    if (request == NULL)
        return NULL;
    peer = clawt_fed_registry_by_fleet(self->fed_registry, request->from);
    if (answer->status == 200 && peer != NULL && peer->seal_key != NULL
        && clawt_config_get_enum(self->config, "federation.seal") == CLAWT_FED_SEAL_ALWAYS) {
        guint8 seal[CLAWT_FLEET_KEY_SIZE];
        if (clawt_fed_decode_key(peer->seal_key, seal)) {
            gchar *sealed = clawt_fed_seal(self->fed_key, seal, answer->body, NULL);
            if (sealed != NULL) {
                g_free(answer->body);
                answer->body = sealed;
            }
        }
    }
    binding = g_strdup_printf("re:%s", request->nonce);
    return clawt_fed_stamp_sign(self->fed_key, self->fed_key_index, "reply", self->fed_fleet_id,
                                request->from, answer->body, binding, NULL);
}

static void
on_http(
    ClawtFedServer *server,
    ClawtFedRoute   route,
    ClawtFedStamp  *stamp,
    const gchar    *body,
    const gchar    *remote,
    ClawtFedAnswer *out,
    gpointer        user_data
){
    ClawtDaemon *self = user_data;
    Answer answer = { 500, NULL, NULL };

    (void)server;
    if (route == CLAWT_FED_ROUTE_HELLO && stamp == NULL) {
        /* Public: who this fleet is, for a probe or `discover`. */
        g_autoptr(JsonObject) me = self_description(self);
        out->status = 200;
        out->body = object_text(me);
        out->stamp = clawt_fed_stamp_sign(self->fed_key, self->fed_key_index, "reply",
                                          self->fed_fleet_id, "", out->body, "re:hello", NULL);
        return;
    }
    receive(self, stamp, body, clawt_fed_route_binding(route),
            route == CLAWT_FED_ROUTE_INVITE, &answer);
    audit(self, "in", answer.peer_alias != NULL ? answer.peer_alias : remote,
          stamp != NULL ? stamp->kind : "?", answer.status == 200 ? "ok" : "refused",
          answer.status == 200 ? "http" : answer.body);
    out->stamp = sign_answer(self, stamp, &answer);
    out->status = answer.status;
    out->body = answer.body;
    g_free(answer.peer_alias);
}

static gboolean
on_socket(
    ClawtFedServer          *server,
    SoupWebsocketConnection *connection,
    ClawtFedStamp           *stamp,
    gpointer                 user_data
){
    (void)server;
    (void)connection;
    (void)stamp;
    (void)user_data;
    /* The live stream arrives with mirroring; until then, refused. */
    return FALSE;
}

static gint
status_to_code(
    guint status
){
    switch (status) {
    case 401: case 403: return CLAWT_ERROR_AUTH;
    case 404: return CLAWT_ERROR_NOT_FOUND;
    case 409: return CLAWT_ERROR_ALREADY_EXISTS;
    case 429: return CLAWT_ERROR_LOOP_LIMIT;
    case 501: return CLAWT_ERROR_NOT_SUPPORTED;
    default:  return CLAWT_ERROR_INVALID_ARGUMENT;
    }
}

/* The simple daemon connection: fed.* frames on the IPC port. */
static JsonNode *
on_ipc_federation(
    ClawtIpcServer *server,
    JsonNode       *request,
    const gchar    *peer_fleet,
    gchar         **out_peer_fleet,
    gpointer        user_data
){
    ClawtDaemon *self = user_data;
    const gchar *kind = clawt_ipc_frame_get_kind(request);
    JsonObject *payload = clawt_ipc_frame_get_payload(request);
    g_autoptr(ClawtFedStamp) stamp = NULL;
    g_autofree gchar *body = NULL;
    Answer answer = { 500, NULL, NULL };
    JsonNode *reply;

    (void)server;
    (void)peer_fleet;
    if (!fed_on(self) || !clawt_config_get_boolean(self->config, "federation.ipc"))
        return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
                                   "this daemon does not federate over its IPC port");

    stamp = clawt_fed_envelope_split(payload, &body, NULL);
    if (stamp == NULL)
        return clawt_ipc_error_new(request, CLAWT_ERROR_AUTH, "unsigned");

    if (g_strcmp0(kind, "fed.hello") == 0) {
        ClawtFedPeer *peer = clawt_fed_registry_by_fleet(self->fed_registry, stamp->from);
        guint8 key[CLAWT_FLEET_KEY_SIZE];
        g_autoptr(JsonBuilder) b = json_builder_new();
        gint64 now = now_s();

        /*
         * An unknown fleet may still say hello -- it is about to redeem an
         * invite -- but stays a nobody: only signed fed.invite and
         * fed.envelope frames do anything, and each is checked alone.
         */
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "fleet");
        json_builder_add_string_value(b, self->fed_fleet_id);
        json_builder_set_member_name(b, "known");
        if (peer != NULL && g_strcmp0(peer->status, "revoked") != 0
            && clawt_fed_peer_public_key(peer, stamp->key_index, now, key)
            && clawt_fed_stamp_check(stamp, key, self->fed_fleet_id, body, "IPC fed.hello", now,
                                     (guint)clawt_config_get_int(self->config,
                                                                 "federation.clock_skew_seconds"),
                                     NULL)
            && clawt_fed_replay_guard_admit(self->fed_guard, stamp->from, stamp->nonce,
                                            stamp->time, now)) {
            PeerState *ps = state_of(self, peer->alias);
            json_builder_add_boolean_value(b, TRUE);
            *out_peer_fleet = g_strdup(stamp->from);
            if (ps != NULL)
                ps->last_seen = now;
            audit(self, "in", peer->alias, "hello", "ok", "ipc");
        } else {
            json_builder_add_boolean_value(b, FALSE);
        }
        json_builder_end_object(b);
        return clawt_ipc_response_new(request, json_builder_get_root(b));
    }

    if (g_strcmp0(kind, "fed.envelope") == 0)
        receive(self, stamp, body, "IPC fed.envelope", FALSE, &answer);
    else if (g_strcmp0(kind, "fed.invite") == 0)
        receive(self, stamp, body, "IPC fed.invite", TRUE, &answer);
    else
        answer_error(&answer, 501, "unknown federation frame");

    audit(self, "in", answer.peer_alias != NULL ? answer.peer_alias : stamp->from, stamp->kind,
          answer.status == 200 ? "ok" : "refused", answer.status == 200 ? "ipc" : answer.body);

    if (answer.status == 200) {
        g_autoptr(ClawtFedStamp) signed_answer = sign_answer(self, stamp, &answer);
        JsonNode *node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(node, clawt_fed_envelope_build(signed_answer, answer.body));
        reply = clawt_ipc_response_new(request, node);
    } else {
        g_autoptr(JsonObject) err = parse_object(answer.body);
        reply = clawt_ipc_error_new(request, status_to_code(answer.status),
                                    obj_str(err, "error") != NULL ? obj_str(err, "error")
                                                                  : "refused");
    }
    g_free(answer.body);
    g_free(answer.peer_alias);
    return reply;
}

/* ---- links: dialling the peers that have an endpoint ------------------ */

static void ping_peer(ClawtDaemon *self, PeerState *ps, ClawtIpcPending *pending);

static void
on_link_push(
    ClawtFedLink *link,
    const gchar  *kind,
    const gchar  *body,
    gpointer      user_data
){
    PeerState *ps = user_data;

    (void)link;
    (void)body;
    ps->last_seen = now_s();
    audit(ps->daemon, "in", ps->alias, kind, "ok", "push");
}

static ClawtFedLink *
make_link(
    ClawtDaemon   *self,
    ClawtFedPeer  *peer,
    GError       **error
){
    ClawtFedLink *link;
    ClawtFedPath path;
    /* A pinned certificate means the peer's IPC port speaks TLS. */
    gboolean tls_ipc = peer->cert_pin != NULL;
    g_autofree gchar *access_file = NULL;
    g_autofree gchar *access_secret = NULL;

    if (!clawt_fed_transport_check(peer->endpoint,
                                   clawt_config_get_boolean(self->config,
                                                            "federation.allow_lan_plaintext"),
                                   tls_ipc, &path, error))
        return NULL;
    link = clawt_fed_link_new(self->fed_key, self->fed_key_index, self->fed_fleet_id,
                              peer->endpoint, self->main_context, error);
    if (link == NULL)
        return NULL;
    clawt_fed_link_set_peer(link, peer);
    clawt_fed_link_set_tls_pin(link, peer->cert_pin);
    clawt_fed_link_set_seal(link, peer->seal_key != NULL
                            && clawt_fed_should_seal(
                                (ClawtFedSeal)clawt_config_get_enum(self->config, "federation.seal"),
                                path));
    access_file = secrets_path(self, peer->alias);
    if (access_file != NULL && peer->access_id != NULL) {
        g_autofree gchar *file = g_strconcat(access_file, ".cf-access", NULL);
        if (g_file_get_contents(file, &access_secret, NULL, NULL)) {
            g_strstrip(access_secret);
            clawt_fed_link_set_access(link, peer->access_id, access_secret);
            memset(access_secret, 0, strlen(access_secret));
        }
    }
    return link;
}

void
clawt_daemon_federation_refresh_links(
    ClawtDaemon *self
){
    g_autoptr(GPtrArray) peers = NULL;
    GHashTableIter iter;
    gpointer key;
    gpointer value;
    guint i;

    if (!fed_on(self))
        return;
    peers = clawt_fed_registry_list(self->fed_registry);

    /* Drop state for peers that are gone. */
    g_hash_table_iter_init(&iter, self->fed_links);
    while (g_hash_table_iter_next(&iter, &key, &value))
        if (clawt_fed_registry_get(self->fed_registry, key) == NULL)
            g_hash_table_iter_remove(&iter);

    for (i = 0; i < peers->len; i++) {
        ClawtFedPeer *peer = g_ptr_array_index(peers, i);
        PeerState *ps = state_of(self, peer->alias);
        gboolean wants_link = peer->endpoint != NULL && g_strcmp0(peer->status, "revoked") != 0;
        g_autoptr(GError) error = NULL;

        if (ps == NULL) {
            ps = g_new0(PeerState, 1);
            ps->daemon = self;
            ps->alias = g_strdup(peer->alias);
            ps->health = CLAWT_FED_HEALTH_PENDING;
            g_hash_table_insert(self->fed_links, g_strdup(peer->alias), ps);
        }
        if (g_strcmp0(peer->status, "revoked") == 0 && self->ipc_server != NULL)
            clawt_ipc_server_close_fleet(self->ipc_server, peer->fleet_id);

        /* Rebuilt when the endpoint or keys changed: simplest to always. */
        if (ps->link != NULL) {
            g_signal_handlers_disconnect_by_data(ps->link, ps);
            clawt_fed_link_close(ps->link);
            g_clear_object(&ps->link);
        }
        if (!wants_link)
            continue;
        ps->link = make_link(self, peer, &error);
        if (ps->link == NULL) {
            set_health(ps, CLAWT_FED_HEALTH_OFFLINE, error->message);
            continue;
        }
        g_signal_connect(ps->link, "push", G_CALLBACK(on_link_push), ps);
        ps->next_try = 0;
        ps->backoff = 0;
    }
}

typedef struct {
    ClawtDaemon     *daemon;
    gchar           *alias;
    gint64           started;
    ClawtIpcPending *pending;
} PingCall;

static void
on_ping(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    PingCall *call = user_data;
    ClawtDaemon *self = call->daemon;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *body = clawt_fed_link_request_finish(CLAWT_FED_LINK(source), result, &error);
    PeerState *ps = fed_on(self) ? state_of(self, call->alias) : NULL;
    ClawtFedPeer *peer = fed_on(self) ? clawt_fed_registry_get(self->fed_registry, call->alias) : NULL;
    gint64 now = now_s();

    if (ps != NULL) {
        ps->in_flight = FALSE;
        if (body != NULL) {
            g_autoptr(JsonObject) o = parse_object(body);
            JsonNode *grant = o != NULL ? json_object_get_member(o, "grant") : NULL;

            ps->last_ok = now;
            ps->misses = 0;
            ps->backoff = 0;
            ps->latency_ms = (g_get_monotonic_time() - call->started) / 1000;
            ps->next_try = now + clawt_config_get_int(self->config, "federation.ping_seconds");
            set_health(ps, CLAWT_FED_HEALTH_LIVE, NULL);
            /* What it allows us, shown on the peer page, never enforced here. */
            if (peer != NULL && grant != NULL && JSON_NODE_HOLDS_OBJECT(grant)) {
                if (peer->their_grant != NULL)
                    json_object_unref(peer->their_grant);
                peer->their_grant = json_object_ref(json_node_get_object(grant));
                save_registry(self);
            }
            audit(self, "out", call->alias, "hello", "ok", NULL);
        } else {
            guint max = (guint)MAX(clawt_config_get_int(self->config, "federation.reconnect_max_seconds"), 1);
            ps->misses++;
            ps->backoff = ps->backoff == 0 ? 1 : MIN(ps->backoff * 2, max);
            ps->next_try = now + ps->backoff;
            if (error->code == CLAWT_ERROR_AUTH && error->domain == CLAWT_ERROR)
                set_health(ps, CLAWT_FED_HEALTH_OFFLINE, error->message);
            else if (ps->misses >= 3 || ps->health != CLAWT_FED_HEALTH_LIVE)
                set_health(ps, CLAWT_FED_HEALTH_OFFLINE, error->message);
            audit(self, "out", call->alias, "hello", "failed", error->message);
        }
    }

    if (call->pending != NULL) {
        JsonNode *request = clawt_ipc_pending_get_request(call->pending);
        if (body != NULL) {
            g_autoptr(JsonBuilder) b = json_builder_new();
            json_builder_begin_object(b);
            json_builder_set_member_name(b, "alias");
            json_builder_add_string_value(b, call->alias);
            json_builder_set_member_name(b, "ms");
            json_builder_add_int_value(b, (g_get_monotonic_time() - call->started) / 1000);
            json_builder_end_object(b);
            clawt_ipc_pending_respond(call->pending,
                                      clawt_ipc_response_new(request, json_builder_get_root(b)));
        } else {
            clawt_ipc_pending_respond(call->pending,
                                      clawt_ipc_error_new(request, error->code, error->message));
        }
    }
    g_object_unref(call->daemon);
    g_free(call->alias);
    g_free(call);
}

static void
ping_peer(
    ClawtDaemon     *self,
    PeerState       *ps,
    ClawtIpcPending *pending
){
    PingCall *call;
    g_autoptr(JsonObject) me = NULL;
    g_autofree gchar *body = NULL;

    if (ps->link == NULL) {
        if (pending != NULL)
            clawt_ipc_pending_respond(pending, clawt_ipc_error_new(
                clawt_ipc_pending_get_request(pending), CLAWT_ERROR_NOT_CONNECTED,
                "this fleet has no endpoint; it dials this one"));
        return;
    }
    call = g_new0(PingCall, 1);
    call->daemon = g_object_ref(self);
    call->alias = g_strdup(ps->alias);
    call->started = g_get_monotonic_time();
    call->pending = pending;
    if (pending == NULL) {
        ps->in_flight = TRUE;
        if (ps->health == CLAWT_FED_HEALTH_PENDING || ps->health == CLAWT_FED_HEALTH_OFFLINE)
            set_health(ps, CLAWT_FED_HEALTH_CONNECTING, ps->error);
    }
    /* A hello: it keeps the peer's view of us current as it checks the link. */
    me = self_description(self);
    body = object_text(me);
    clawt_fed_link_request_async(ps->link, "hello", body, NULL, on_ping, call);
}

static gboolean
on_tick(
    gpointer user_data
){
    ClawtDaemon *self = user_data;
    GHashTableIter iter;
    gpointer value;
    gint64 now = now_s();
    static guint ticks = 0;

    if (!fed_on(self))
        return G_SOURCE_CONTINUE;

    g_hash_table_iter_init(&iter, self->fed_links);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        PeerState *ps = value;
        ClawtFedPeer *peer = clawt_fed_registry_get(self->fed_registry, ps->alias);

        if (peer == NULL || ps->link == NULL || ps->in_flight || now < ps->next_try)
            continue;
        if (g_strcmp0(peer->status, "revoked") == 0)
            continue;
        ping_peer(self, ps, NULL);
    }

    /* Housekeeping, about once an hour. */
    if (++ticks % 3600 == 1) {
        if (clawt_fed_registry_prune_invites(self->fed_registry, now) > 0)
            save_registry(self);
        audit_prune(self);
    }
    {
        g_autofree gchar *path = state_path(self, "nonces");
        clawt_fed_replay_guard_save(self->fed_guard, path, now, NULL);
    }
    return G_SOURCE_CONTINUE;
}

/* ---- start and stop ---------------------------------------------------- */

const gchar *
clawt_daemon_federation_fleet_id(
    ClawtDaemon *self
){
    return self->fed_fleet_id;
}

void
clawt_daemon_federation_setup(
    ClawtDaemon *self
){
    g_autoptr(GError) error = NULL;
    g_autofree gchar *key_path = NULL;
    g_autofree gchar *peers_path = NULL;
    g_autofree gchar *invites_path = NULL;
    g_autofree gchar *nonces = NULL;
    g_autofree gchar *dir = NULL;
    g_auto(GStrv) listen = NULL;
    guint i;

    if (!clawt_config_get_boolean(self->config, "federation.enabled") || self->state_dir == NULL)
        return;

    key_path = secrets_path(self, "fleet.ed25519");
    if (key_path == NULL) {
        g_warning("federation: secrets.dir is not set; federation is off");
        return;
    }
    self->fed_key = clawt_fleet_key_load_or_create(key_path, &error);
    if (self->fed_key == NULL) {
        g_warning("federation: no fleet key: %s; federation is off", error->message);
        return;
    }
    load_identity(self);
    if (!save_identity(self, &error)) {
        g_warning("federation: the fleet identity was not saved: %s", error->message);
        g_clear_error(&error);
    }

    dir = g_build_filename(self->state_dir, "federation", NULL);
    clawt_ensure_dir(dir, 0700, NULL);
    peers_path = state_path(self, "peers.yaml");
    invites_path = secrets_path(self, "invites.yaml");
    self->fed_registry = clawt_fed_registry_new(peers_path, invites_path);
    if (!clawt_fed_registry_load(self->fed_registry, &error)) {
        g_warning("federation: %s is unreadable: %s; federation is off", peers_path,
                  error->message);
        g_clear_object(&self->fed_registry);
        g_clear_pointer(&self->fed_key, clawt_fleet_key_unref);
        return;
    }
    self->fed_guard = clawt_fed_replay_guard_new(
        (guint)clawt_config_get_int(self->config, "federation.clock_skew_seconds"));
    nonces = state_path(self, "nonces");
    clawt_fed_replay_guard_load(self->fed_guard, nonces, now_s());
    self->fed_links = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                            (GDestroyNotify)peer_state_free);
    self->fed_audit = g_queue_new();

    /* The HTTP door; a door that will not open is a warning. */
    self->fed_server = clawt_fed_server_new(
        (gsize)clawt_config_get_int(self->config, "federation.max_body_bytes"),
        on_http, on_socket, self);
    listen = clawt_config_get_string_list(self->config, "federation.listen");
    if (listen == NULL || listen[0] == NULL) {
        g_autofree gchar *sock = g_build_filename(self->state_dir, "federation.sock", NULL);
        g_autofree gchar *where = g_strconcat("unix:", sock, NULL);
        if (!clawt_fed_server_listen(self->fed_server, where, &error)) {
            g_warning("federation: cannot listen on %s: %s", where, error->message);
            g_clear_error(&error);
        }
    }
    for (i = 0; listen != NULL && listen[i] != NULL; i++) {
        g_autofree gchar *endpoint = NULL;

        if (!g_str_has_prefix(listen[i], "unix:")) {
            /* The listener speaks plain HTTP: the same rule as a link. */
            endpoint = g_strdup_printf("http://%s", listen[i]);
            if (!clawt_fed_transport_check(endpoint,
                                           clawt_config_get_boolean(self->config,
                                                                    "federation.allow_lan_plaintext"),
                                           FALSE, NULL, &error)) {
                g_warning("federation: not listening on %s: %s", listen[i], error->message);
                g_clear_error(&error);
                continue;
            }
        }
        if (!clawt_fed_server_listen(self->fed_server, listen[i], &error)) {
            g_warning("federation: cannot listen on %s: %s", listen[i], error->message);
            g_clear_error(&error);
            continue;
        }
        g_message("federation: listening on %s", listen[i]);
    }

    if (self->ipc_server != NULL)
        clawt_ipc_server_set_federation_handler(self->ipc_server, on_ipc_federation, self);

    clawt_daemon_federation_refresh_links(self);
    self->fed_tick = clawt_timeout_add_seconds(1, on_tick, self);
    g_message("federation: this is fleet %s (%s)", self->fed_fleet_id, fleet_name(self));
}

void
clawt_daemon_federation_teardown(
    ClawtDaemon *self
){
    if (self->fed_tick != NULL) {
        g_source_destroy(self->fed_tick);
        g_clear_pointer(&self->fed_tick, g_source_unref);
    }
    if (self->ipc_server != NULL)
        clawt_ipc_server_set_federation_handler(self->ipc_server, NULL, NULL);
    if (self->fed_guard != NULL && self->state_dir != NULL) {
        g_autofree gchar *nonces = state_path(self, "nonces");
        clawt_fed_replay_guard_save(self->fed_guard, nonces, now_s(), NULL);
    }
    g_clear_pointer(&self->fed_links, g_hash_table_unref);
    if (self->fed_server != NULL)
        clawt_fed_server_stop(self->fed_server);
    g_clear_object(&self->fed_server);
    g_clear_pointer(&self->fed_guard, clawt_fed_replay_guard_unref);
    g_clear_object(&self->fed_registry);
    g_clear_pointer(&self->fed_key, clawt_fleet_key_unref);
    g_clear_pointer(&self->fed_fleet_id, g_free);
    if (self->fed_audit != NULL)
        g_queue_free_full(g_steal_pointer(&self->fed_audit), (GDestroyNotify)json_object_unref);
}

/* ---- the operator's requests ------------------------------------------ */

static JsonObject *
peer_listing(
    ClawtDaemon  *self,
    ClawtFedPeer *peer
){
    ClawtFedHealth health = health_of(self, peer);
    JsonObject *o = clawt_fed_peer_selector_json(peer, health);
    PeerState *ps = state_of(self, peer->alias);
    g_autofree gchar *words = NULL;

    json_object_set_string_member(o, "tier_label", clawt_fed_tier_label(peer->tier));
    json_object_set_string_member(o, "health_label", clawt_fed_health_label(health));
    if (peer->owner == NULL)
        json_object_set_string_member(o, "owner", "");
    words = clawt_fed_safety_words(peer->fleet_id);
    json_object_set_string_member(o, "safety_words", words);
    if (peer->endpoint != NULL) {
        ClawtFedPath path;
        ClawtFedTransport transport = CLAWT_FED_TRANSPORT_HTTP;
        if (clawt_fed_transport_check(peer->endpoint, TRUE, peer->cert_pin != NULL, &path, NULL))
            json_object_set_string_member(o, "path", clawt_fed_path_label(path));
        clawt_fed_endpoint_parse(peer->endpoint, &transport, NULL, NULL, NULL, NULL);
        json_object_set_string_member(o, "transport",
                                      transport == CLAWT_FED_TRANSPORT_IPC ? "ipc" : "http");
    } else {
        json_object_set_string_member(o, "transport", "inbound");
    }
    if (ps != NULL) {
        if (ps->last_ok > 0)
            json_object_set_int_member(o, "last_ok", ps->last_ok);
        if (ps->last_seen > 0)
            json_object_set_int_member(o, "last_seen", ps->last_seen);
        if (ps->latency_ms > 0)
            json_object_set_int_member(o, "latency_ms", ps->latency_ms);
        if (ps->error != NULL)
            json_object_set_string_member(o, "error", ps->error);
    }
    json_object_set_int_member(o, "created_at", peer->created_at);
    return o;
}

static JsonNode *
reply_object(
    JsonNode   *request,
    JsonObject *object
){
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);

    json_node_take_object(node, object);
    return clawt_ipc_response_new(request, node);
}

static JsonNode *
handle_status(
    ClawtDaemon *self,
    JsonNode    *request
){
    JsonObject *o = json_object_new();
    g_autofree gchar *endpoint = NULL;
    g_autofree gchar *words = NULL;

    json_object_set_boolean_member(o, "enabled", fed_on(self));
    if (!fed_on(self)) {
        json_object_set_string_member(o, "hint",
            "set federation.enabled: true and restart the daemon "
            "(clawtilla config set federation.enabled true)");
        return reply_object(request, o);
    }
    words = clawt_fed_safety_words(self->fed_fleet_id);
    endpoint = default_endpoint(self);
    json_object_set_string_member(o, "fleet_id", self->fed_fleet_id);
    json_object_set_string_member(o, "name", fleet_name(self));
    json_object_set_string_member(o, "owner",
        clawt_config_get_string(self->config, "federation.owner") != NULL
            ? clawt_config_get_string(self->config, "federation.owner") : "");
    json_object_set_string_member(o, "safety_words", words);
    json_object_set_int_member(o, "key_index", self->fed_key_index);
    if (endpoint != NULL)
        json_object_set_string_member(o, "endpoint", endpoint);
    json_object_set_boolean_member(o, "ipc", clawt_config_get_boolean(self->config, "federation.ipc"));
    {
        g_autofree gchar *pin = our_tls_pin(self);
        if (pin != NULL)
            json_object_set_string_member(o, "tls_pin", pin);
    }
    {
        g_auto(GStrv) bound = clawt_fed_server_list_bound(self->fed_server);
        JsonArray *a = json_array_new();
        guint i;
        for (i = 0; bound[i] != NULL; i++)
            json_array_add_string_element(a, bound[i]);
        json_object_set_array_member(o, "listening", a);
    }
    {
        g_autoptr(GPtrArray) peers = clawt_fed_registry_list(self->fed_registry);
        g_autoptr(GPtrArray) invites = clawt_fed_registry_list_invites(self->fed_registry);
        json_object_set_int_member(o, "peers", peers->len);
        json_object_set_int_member(o, "invites", invites->len);
    }
    return reply_object(request, o);
}

static JsonNode *
handle_invite_create(
    ClawtDaemon *self,
    JsonNode    *request,
    JsonObject  *payload
){
    g_autoptr(ClawtFedInvite) invite = g_new0(ClawtFedInvite, 1);
    g_autoptr(GError) error = NULL;
    ClawtFedPendingInvite *pending;
    ClawtFedTier tier = CLAWT_FED_TIER_LIMITED;
    const gchar *tier_nick = clawt_ipc_payload_string(payload, "tier");
    const gchar *endpoint = clawt_ipc_payload_string(payload, "endpoint");
    const gchar *access_id = clawt_ipc_payload_string(payload, "access_id");
    const gchar *access_secret = clawt_ipc_payload_string(payload, "access_secret");
    gint64 hours = clawt_ipc_payload_int(payload, "expires_hours",
                                         clawt_config_get_int(self->config, "federation.invite_ttl_hours"));
    g_autofree gchar *chosen = NULL;
    g_autofree gchar *qr = NULL;
    g_autofree gchar *words = NULL;
    JsonObject *o;

    if (tier_nick != NULL && !clawt_fed_tier_from_nick(tier_nick, &tier))
        return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                   "the tiers are owned, trusted, limited and untrusted");
    chosen = endpoint != NULL && *endpoint != '\0' ? g_strdup(endpoint) : default_endpoint(self);
    if (chosen == NULL)
        return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
            "this fleet has no address another fleet can reach: set "
            "federation.public_url (a Cloudflare tunnel's https:// address, or "
            "clawtilla://TAILNET-IP:PORT), or put the daemon on a tailnet");
    if (!clawt_fed_endpoint_check(chosen, &error))
        return clawt_ipc_error_new(request, error->code, error->message);
    if (hours < 1 || hours > 24 * 30)
        return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                   "an invite lasts between 1 hour and 30 days");
    if ((access_id == NULL) != (access_secret == NULL))
        return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                   "a Cloudflare Access token is an id and a secret");

    invite->endpoint = g_strdup(chosen);
    invite->fleet_id = g_strdup(self->fed_fleet_id);
    invite->invite_id = clawt_fed_random_text(9);
    invite->secret = clawt_fed_random_text(24);
    if (invite->invite_id == NULL || invite->secret == NULL)
        return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED, "/dev/urandom is unreadable");
    invite->tier = tier;
    invite->name = g_strdup(fleet_name(self));
    invite->owner = g_strdup(clawt_config_get_string(self->config, "federation.owner"));
    invite->expires = now_s() + hours * 3600;
    invite->access_id = g_strdup(access_id);
    invite->access_secret = g_strdup(access_secret);
    if (g_str_has_prefix(chosen, "clawtilla://"))
        invite->tls_pin = our_tls_pin(self);

    pending = g_new0(ClawtFedPendingInvite, 1);
    pending->id = g_strdup(invite->invite_id);
    pending->secret = g_strdup(invite->secret);
    pending->tier = tier;
    pending->expires = invite->expires;
    pending->confirm = clawt_ipc_payload_boolean(payload, "confirm", FALSE);
    pending->note = g_strdup(clawt_ipc_payload_string(payload, "note"));
    pending->created_at = now_s();
    clawt_fed_registry_add_invite(self->fed_registry, pending);
    save_registry(self);

    o = json_object_new();
    {
        g_autofree gchar *uri = clawt_fed_invite_to_uri(invite);
        g_autofree gchar *paste = clawt_fed_invite_to_paste(invite);
        json_object_set_string_member(o, "uri", uri);
        json_object_set_string_member(o, "paste", paste);
        qr = clawt_qr_render_svg(uri, NULL);
        if (qr != NULL)
            json_object_set_string_member(o, "qr_svg", qr);
    }
    words = clawt_fed_safety_words(self->fed_fleet_id);
    json_object_set_string_member(o, "id", invite->invite_id);
    json_object_set_string_member(o, "fleet_id", self->fed_fleet_id);
    json_object_set_string_member(o, "safety_words", words);
    json_object_set_string_member(o, "endpoint", invite->endpoint);
    json_object_set_string_member(o, "tier", clawt_fed_tier_nick(tier));
    json_object_set_int_member(o, "expires", invite->expires);
    json_object_set_boolean_member(o, "confirm", pending->confirm);
    audit(self, "local", NULL, "invite.create", "ok", clawt_fed_tier_nick(tier));
    return reply_object(request, o);
}

static JsonObject *
invite_preview(
    ClawtDaemon    *self,
    ClawtFedInvite *invite
){
    JsonObject *o = json_object_new();
    g_autoptr(GError) error = NULL;
    ClawtFedPath path;
    gboolean ok;

    json_object_set_string_member(o, "name", invite->name != NULL ? invite->name : "");
    json_object_set_string_member(o, "owner", invite->owner != NULL ? invite->owner : "");
    json_object_set_string_member(o, "endpoint", invite->endpoint);
    json_object_set_string_member(o, "fleet_id", invite->fleet_id);
    {
        g_autofree gchar *words = clawt_fed_safety_words(invite->fleet_id);
        json_object_set_string_member(o, "safety_words", words);
    }
    json_object_set_string_member(o, "tier", clawt_fed_tier_nick(invite->tier));
    json_object_set_string_member(o, "tier_summary", clawt_fed_tier_summary(invite->tier));
    json_object_set_int_member(o, "expires", invite->expires);
    json_object_set_boolean_member(o, "expired", invite->expires < now_s());
    json_object_set_boolean_member(o, "access_token", invite->access_id != NULL);
    json_object_set_boolean_member(o, "tls_pinned", invite->tls_pin != NULL);
    {
        g_autofree gchar *alias = clawt_fed_alias_suggest(invite->name);
        json_object_set_string_member(o, "suggested_alias", alias);
    }
    ok = clawt_fed_transport_check(invite->endpoint,
                                   self->config != NULL
                                       && clawt_config_get_boolean(self->config,
                                                                   "federation.allow_lan_plaintext"),
                                   invite->tls_pin != NULL, &path, &error);
    json_object_set_boolean_member(o, "reachable", ok);
    if (ok)
        json_object_set_string_member(o, "path", clawt_fed_path_label(path));
    else
        json_object_set_string_member(o, "problem", error->message);
    if (fed_on(self)) {
        ClawtFedPeer *known = clawt_fed_registry_by_fleet(self->fed_registry, invite->fleet_id);
        if (known != NULL)
            json_object_set_string_member(o, "already_linked_as", known->alias);
        if (g_strcmp0(invite->fleet_id, self->fed_fleet_id) == 0)
            json_object_set_string_member(o, "problem", "this invite is from this fleet");
    }
    return o;
}

typedef struct {
    ClawtDaemon     *daemon;
    ClawtIpcPending *pending;
    ClawtFedLink    *link;
    ClawtFedInvite  *invite;
    gchar           *alias;
    gchar          **tags;
    ClawtFedTier     tier;
    gint64           sent_at;
} Accepting;

static void
accepting_free(
    Accepting *a
){
    if (a->link != NULL)
        clawt_fed_link_close(a->link);
    g_clear_object(&a->link);
    clawt_fed_invite_free(a->invite);
    g_free(a->alias);
    g_strfreev(a->tags);
    g_object_unref(a->daemon);
    g_free(a);
}

static void
on_accepted(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    Accepting *a = user_data;
    ClawtDaemon *self = a->daemon;
    JsonNode *request = clawt_ipc_pending_get_request(a->pending);
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtFedExchange) exchange = NULL;
    g_autoptr(JsonObject) o = NULL;
    g_autofree gchar *binding = NULL;
    g_autofree gchar *fingerprint = NULL;
    ClawtFedPeer *peer;
    const gchar *key;
    guint8 raw[CLAWT_FLEET_KEY_SIZE];
    gint64 now = now_s();

    exchange = clawt_fed_link_exchange_finish(CLAWT_FED_LINK(source), result, &error);
    if (!fed_on(self)) {
        clawt_ipc_pending_respond(a->pending, clawt_ipc_error_new(request, CLAWT_ERROR_AGENT_STATE,
                                                                  "federation stopped"));
        accepting_free(a);
        return;
    }
    if (exchange == NULL) {
        audit(self, "out", a->alias, "invite.accept", "failed", error->message);
        clawt_ipc_pending_respond(a->pending, clawt_ipc_error_new(request, error->code,
                                                                  error->message));
        accepting_free(a);
        return;
    }
    if (exchange->status != 200) {
        g_autoptr(GError) refused = clawt_fed_answer_error(exchange->status, exchange->body);
        audit(self, "out", a->alias, "invite.accept", "refused", refused->message);
        clawt_ipc_pending_respond(a->pending, clawt_ipc_error_new(request, refused->code,
                                                                  refused->message));
        accepting_free(a);
        return;
    }

    /*
     * The fleet that answered must be the one the invite named: its key's
     * fingerprint is the invite's, and it signed this answer, bound to
     * our request.  This is the fingerprint check, done for the person.
     */
    o = parse_object(exchange->body);
    key = obj_str(o, "key");
    binding = g_strdup_printf("re:%s", exchange->request_nonce);
    if (key == NULL || !clawt_fed_decode_key(key, raw)
        || (fingerprint = clawt_fleet_key_fingerprint(raw)) == NULL
        || g_strcmp0(fingerprint, a->invite->fleet_id) != 0
        || exchange->stamp == NULL
        || !clawt_fed_stamp_check(exchange->stamp, raw, self->fed_fleet_id, exchange->body,
                                  binding, now, CLAWT_FED_DEFAULT_SKEW_SECONDS, &error)) {
        audit(self, "out", a->alias, "invite.accept", "refused", "fingerprint mismatch");
        clawt_ipc_pending_respond(a->pending, clawt_ipc_error_new(request, CLAWT_ERROR_AUTH,
            "the fleet that answered is not the one that made the invite (its "
            "fingerprint does not match); nothing was linked"));
        accepting_free(a);
        return;
    }

    peer = clawt_fed_peer_new(a->alias, a->invite->fleet_id, key);
    peer->label = bounded(obj_str(o, "name"), 64);
    if (peer->label == NULL)
        peer->label = g_strdup(a->invite->name);
    peer->owner = bounded(obj_str(o, "owner"), 64);
    peer->version = bounded(obj_str(o, "version"), 32);
    peer->tier = a->tier;
    peer->endpoint = g_strdup(a->invite->endpoint);
    peer->cert_pin = g_strdup(a->invite->tls_pin);
    if (obj_str(o, "seal_key") != NULL && clawt_fed_decode_key(obj_str(o, "seal_key"), raw))
        peer->seal_key = g_strdup(obj_str(o, "seal_key"));
    if (a->tags != NULL) {
        g_strfreev(peer->tags);
        peer->tags = g_strdupv(a->tags);
    }
    if (a->invite->access_id != NULL) {
        g_autofree gchar *base = secrets_path(self, a->alias);
        g_autofree gchar *file = g_strconcat(base, ".cf-access", NULL);
        peer->access_id = g_strdup(a->invite->access_id);
        clawt_write_file_atomic(file, a->invite->access_secret, -1, 0600, FALSE, NULL);
    }
    {
        ClawtFedPeer *old = clawt_fed_registry_by_fleet(self->fed_registry, peer->fleet_id);
        if (old != NULL && g_strcmp0(old->status, "revoked") == 0)
            clawt_fed_registry_remove(self->fed_registry, old->alias);
    }
    if (!clawt_fed_registry_add(self->fed_registry, peer, &error)) {
        clawt_ipc_pending_respond(a->pending, clawt_ipc_error_new(request, error->code,
                                                                  error->message));
        accepting_free(a);
        return;
    }
    save_registry(self);
    clawt_daemon_federation_refresh_links(self);
    publish_peer(self, a->alias, "added");
    audit(self, "out", a->alias, "invite.accept", "ok", clawt_fed_tier_nick(a->tier));

    {
        ClawtFedPeer *added = clawt_fed_registry_get(self->fed_registry, a->alias);
        JsonObject *reply = peer_listing(self, added);
        json_object_set_string_member(reply, "their_status",
                                      obj_str(o, "status") != NULL ? obj_str(o, "status") : "active");
        clawt_ipc_pending_respond(a->pending, reply_object(request, reply));
    }
    accepting_free(a);
}

/* Shared by `federation.invite.accept` and `federation.link`. */
static JsonNode *
start_accept(
    ClawtDaemon     *self,
    JsonNode        *request,
    ClawtIpcPending *pending,
    ClawtFedInvite  *invite,
    ClawtFedTier     tier,
    const gchar     *alias_in,
    gchar          **tags
){
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonObject) body = NULL;
    g_autofree gchar *alias = NULL;
    g_autofree gchar *text = NULL;
    g_autofree gchar *proof = NULL;
    g_autoptr(JsonObject) me = NULL;
    Accepting *a;
    gint64 now = now_s();
    guint i;

    if (invite->expires < now)
        return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                   "this invite has expired; ask for a new one");
    if (g_strcmp0(invite->fleet_id, self->fed_fleet_id) == 0)
        return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                   "this invite is from this fleet");
    {
        ClawtFedPeer *known = clawt_fed_registry_by_fleet(self->fed_registry, invite->fleet_id);
        if (known != NULL && g_strcmp0(known->status, "revoked") != 0)
            return clawt_ipc_error_new(request, CLAWT_ERROR_ALREADY_EXISTS,
                                       "that fleet is already linked here");
    }
    if (!clawt_fed_transport_check(invite->endpoint,
                                   clawt_config_get_boolean(self->config,
                                                            "federation.allow_lan_plaintext"),
                                   invite->tls_pin != NULL, NULL, &error))
        return clawt_ipc_error_new(request, error->code, error->message);

    alias = alias_in != NULL && *alias_in != '\0' ? g_strdup(alias_in)
                                                  : clawt_fed_alias_suggest(invite->name);
    if (!clawt_fed_alias_check(alias, &error))
        return clawt_ipc_error_new(request, error->code, error->message);
    if (clawt_fed_registry_get(self->fed_registry, alias) != NULL
        && g_strcmp0(clawt_fed_registry_get(self->fed_registry, alias)->status, "revoked") != 0)
        return clawt_ipc_error_new(request, CLAWT_ERROR_ALREADY_EXISTS,
                                   "a peer already has that alias; pass another");
    for (i = 0; tags != NULL && tags[i] != NULL; i++)
        if (*tags[i] == '\0' || strlen(tags[i]) > 64)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "a tag is 1 to 64 characters");

    a = g_new0(Accepting, 1);
    a->daemon = g_object_ref(self);
    a->link = clawt_fed_link_new(self->fed_key, self->fed_key_index, self->fed_fleet_id,
                                 invite->endpoint, self->main_context, &error);
    if (a->link == NULL) {
        g_object_unref(a->daemon);
        g_free(a);
        return clawt_ipc_error_new(request, error->code, error->message);
    }
    if (invite->access_id != NULL)
        clawt_fed_link_set_access(a->link, invite->access_id, invite->access_secret);
    clawt_fed_link_set_tls_pin(a->link, invite->tls_pin);
    a->invite = clawt_fed_invite_copy(invite);
    a->alias = g_steal_pointer(&alias);
    a->tags = g_strdupv(tags);
    a->tier = tier;
    a->pending = pending != NULL ? pending : clawt_ipc_server_defer(self->ipc_server, request);
    if (a->pending == NULL) {
        accepting_free(a);
        return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                   "this request cannot be answered later");
    }

    /* Proof of the secret, bound to our key and to the time we sign at. */
    me = self_description(self);
    body = json_object_ref(me);
    json_object_set_string_member(body, "invite_id", invite->invite_id);
    json_object_set_string_member(body, "tier_given", clawt_fed_tier_nick(tier));
    /* The proof's time travels in the signed body beside it. */
    {
        g_autofree gchar *key = clawt_fleet_key_public_text(self->fed_key);
        proof = clawt_fed_proof(invite->secret, key, invite->fleet_id, now);
        json_object_set_string_member(body, "proof", proof);
        json_object_set_int_member(body, "proof_time", now);
    }
    text = object_text(body);
    a->sent_at = now;
    clawt_fed_link_exchange_async(a->link, clawt_fed_route_binding(CLAWT_FED_ROUTE_INVITE),
                                  "invite.accept", invite->fleet_id, text, NULL, on_accepted, a);
    return NULL;
}

static JsonNode *
handle_invite_accept(
    ClawtDaemon *self,
    JsonNode    *request,
    JsonObject  *payload
){
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtFedInvite) invite = NULL;
    g_auto(GStrv) tags = clawt_ipc_payload_strv(payload, "tags");
    ClawtFedTier tier;
    const gchar *tier_nick = clawt_ipc_payload_string(payload, "tier");

    invite = clawt_fed_invite_parse(clawt_ipc_payload_string(payload, "invite"), &error);
    if (invite == NULL)
        return clawt_ipc_error_new(request, error->code, error->message);
    /* The tier *we* give them; the invite's is only what they give us. */
    if (tier_nick == NULL)
        return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                   "choose the tier this fleet gets here: owned, trusted, "
                                   "limited or untrusted");
    if (!clawt_fed_tier_from_nick(tier_nick, &tier))
        return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                   "the tiers are owned, trusted, limited and untrusted");
    return start_accept(self, request, NULL, invite, tier,
                        clawt_ipc_payload_string(payload, "alias"), tags);
}

/* ---- federation.link: two fleets one person owns ----------------------- */

typedef struct {
    ClawtDaemon     *daemon;
    ClawtIpcPending *pending;
    ClawtClient     *client;
    gchar           *alias;
    gchar          **tags;
    gchar           *endpoint;   /* how we reached it: the link's endpoint too */
} Linking;

static void
linking_free(
    Linking *l
){
    if (l->client != NULL)
        clawt_client_disconnect(l->client);
    g_clear_object(&l->client);
    g_object_unref(l->daemon);
    g_free(l->alias);
    g_strfreev(l->tags);
    g_free(l->endpoint);
    g_free(l);
}

static void
on_link_offer(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    Linking *l = user_data;
    JsonNode *request = clawt_ipc_pending_get_request(l->pending);
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) reply = clawt_client_request_finish(CLAWT_CLIENT(source), result, &error);
    g_autoptr(ClawtFedInvite) invite = NULL;
    JsonObject *o;
    JsonNode *immediate;

    if (reply == NULL || !JSON_NODE_HOLDS_OBJECT(reply)
        || (o = json_node_get_object(reply)) == NULL) {
        clawt_ipc_pending_respond(l->pending, clawt_ipc_error_new(request,
            error != NULL ? error->code : CLAWT_ERROR_FAILED,
            error != NULL ? error->message : "the other daemon made no invite"));
        linking_free(l);
        return;
    }
    invite = clawt_fed_invite_parse(obj_str(o, "paste"), &error);
    if (invite == NULL) {
        clawt_ipc_pending_respond(l->pending, clawt_ipc_error_new(request, error->code,
                                                                  error->message));
        linking_free(l);
        return;
    }
    immediate = start_accept(l->daemon, request, l->pending, invite, CLAWT_FED_TIER_OWNED,
                             l->alias, l->tags);
    if (immediate != NULL)
        clawt_ipc_pending_respond(l->pending, immediate);
    l->pending = NULL;
    linking_free(l);
}

static void
on_link_connected(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    Linking *l = user_data;
    g_autoptr(GError) error = NULL;
    JsonNode *request = clawt_ipc_pending_get_request(l->pending);
    g_autoptr(JsonBuilder) b = json_builder_new();
    g_autofree gchar *note = NULL;

    if (!clawt_client_connect_finish(CLAWT_CLIENT(source), result, &error)) {
        clawt_ipc_pending_respond(l->pending, clawt_ipc_error_new(request, CLAWT_ERROR_NOT_CONNECTED,
                                                                  error->message));
        linking_free(l);
        return;
    }
    note = g_strdup_printf("link from %s", fleet_name(l->daemon));
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "tier");
    json_builder_add_string_value(b, "owned");
    json_builder_set_member_name(b, "note");
    json_builder_add_string_value(b, note);
    json_builder_set_member_name(b, "expires_hours");
    json_builder_add_int_value(b, 1);
    /*
     * The address we just reached it at is the one to keep using: the
     * other daemon may have no public address of its own to offer.
     */
    json_builder_set_member_name(b, "endpoint");
    json_builder_add_string_value(b, l->endpoint);
    json_builder_end_object(b);
    clawt_client_request_async(l->client, "federation.link.offer", json_builder_get_root(b),
                               NULL, on_link_offer, l);
}

static JsonNode *
handle_link(
    ClawtDaemon *self,
    JsonNode    *request,
    JsonObject  *payload
){
    const gchar *target = clawt_ipc_payload_string(payload, "target");
    const gchar *token = clawt_ipc_payload_string(payload, "token");
    Linking *l;

    if (target == NULL || *target == '\0')
        return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                   "name the other daemon: a socket path, or HOST:PORT with "
                                   "its owner token");
    l = g_new0(Linking, 1);
    l->daemon = g_object_ref(self);
    l->alias = g_strdup(clawt_ipc_payload_string(payload, "alias"));
    l->tags = clawt_ipc_payload_strv(payload, "tags");
    if (target[0] == '/') {
        l->client = clawt_client_new(target);
        l->endpoint = g_strdup_printf("unix:%s#ipc", target);
    } else {
        g_autofree gchar *endpoint = g_strdup_printf("clawtilla://%s", target);
        g_autofree gchar *host = NULL;
        guint16 port = 0;
        if (!clawt_fed_endpoint_parse(endpoint, NULL, &host, &port, NULL, NULL)) {
            linking_free(l);
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "the other daemon is a socket path or HOST:PORT");
        }
        l->client = clawt_client_new_tcp(host, port, token);
        l->endpoint = g_steal_pointer(&endpoint);
    }
    l->pending = clawt_ipc_server_defer(self->ipc_server, request);
    if (l->pending == NULL) {
        linking_free(l);
        return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                   "this request cannot be answered later");
    }
    if (self->main_context != NULL)
        g_main_context_push_thread_default(self->main_context);
    clawt_client_connect_async(l->client, NULL, on_link_connected, l);
    if (self->main_context != NULL)
        g_main_context_pop_thread_default(self->main_context);
    return NULL;
}

static JsonNode *
handle_peer_list(
    ClawtDaemon *self,
    JsonNode    *request,
    JsonObject  *payload
){
    g_autoptr(GPtrArray) peers = clawt_fed_registry_list(self->fed_registry);
    const gchar *filter = clawt_ipc_payload_string(payload, "filter");
    gboolean hidden = clawt_ipc_payload_boolean(payload, "include_hidden", TRUE);
    JsonObject *o = json_object_new();
    JsonArray *a = json_array_new();
    guint i;

    if (filter != NULL && *filter != '\0') {
        g_autoptr(GError) error = NULL;
        if (!clawt_fed_selector_check(filter, &error)) {
            json_array_unref(a);
            json_object_unref(o);
            return clawt_ipc_error_new(request, error->code, error->message);
        }
    }
    for (i = 0; i < peers->len; i++) {
        ClawtFedPeer *peer = g_ptr_array_index(peers, i);
        JsonObject *entry = peer_listing(self, peer);

        if ((!hidden && peer->hidden)
            || (filter != NULL && *filter != '\0' && !clawt_fed_selector_matches(filter, entry))) {
            json_object_unref(entry);
            continue;
        }
        json_array_add_object_element(a, entry);
    }
    json_object_set_array_member(o, "peers", a);
    {
        g_autoptr(GPtrArray) invites = clawt_fed_registry_list_invites(self->fed_registry);
        JsonArray *inv = json_array_new();
        for (i = 0; i < invites->len; i++) {
            ClawtFedPendingInvite *p = g_ptr_array_index(invites, i);
            JsonObject *e = json_object_new();
            json_object_set_string_member(e, "id", p->id);
            json_object_set_string_member(e, "tier", clawt_fed_tier_nick(p->tier));
            json_object_set_int_member(e, "expires", p->expires);
            json_object_set_boolean_member(e, "confirm", p->confirm);
            if (p->note != NULL)
                json_object_set_string_member(e, "note", p->note);
            json_array_add_object_element(inv, e);
        }
        json_object_set_array_member(o, "invites", inv);
    }
    return reply_object(request, o);
}

static ClawtFedPeer *
peer_from_payload(
    ClawtDaemon  *self,
    JsonObject   *payload,
    JsonNode     *request,
    JsonNode    **out_error
){
    const gchar *alias = clawt_ipc_payload_string(payload, "alias");
    ClawtFedPeer *peer = clawt_fed_registry_get(self->fed_registry, alias);

    if (peer == NULL)
        *out_error = clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                         "no linked fleet by that alias "
                                         "(clawtilla federation list)");
    return peer;
}

static JsonNode *
handle_peer_show(
    ClawtDaemon *self,
    JsonNode    *request,
    JsonObject  *payload
){
    JsonNode *error = NULL;
    ClawtFedPeer *peer = peer_from_payload(self, payload, request, &error);
    g_autoptr(GHashTable) explain = NULL;
    g_autoptr(GPtrArray) problems = NULL;
    g_autoptr(ClawtFedGrant) grant = NULL;
    JsonObject *o;
    JsonArray *fields;
    JsonArray *notes;
    guint i;

    if (peer == NULL)
        return error;
    o = peer_listing(self, peer);
    grant = grant_for(self, peer, health_of(self, peer), &explain, &problems);
    fields = json_array_new();
    for (i = 0; i < clawt_fed_grant_field_count(); i++) {
        JsonObject *f = json_object_new();
        const gchar *name = clawt_fed_grant_field_name(i);
        g_autofree gchar *value = clawt_fed_grant_get(grant, i);

        json_object_set_string_member(f, "name", name);
        json_object_set_string_member(f, "value", value);
        json_object_set_string_member(f, "source", g_hash_table_lookup(explain, name) != NULL
                                                   ? g_hash_table_lookup(explain, name) : "");
        json_object_set_string_member(f, "doc", clawt_fed_grant_field_doc(i));
        json_object_set_boolean_member(f, "overridden",
                                       peer->overrides != NULL
                                       && json_object_has_member(peer->overrides, name));
        json_array_add_object_element(fields, f);
    }
    json_object_set_array_member(o, "grant", fields);
    notes = json_array_new();
    for (i = 0; i < problems->len; i++)
        json_array_add_string_element(notes, g_ptr_array_index(problems, i));
    json_object_set_array_member(o, "problems", notes);
    if (peer->their_grant != NULL)
        json_object_set_object_member(o, "their_grant", json_object_ref(peer->their_grant));
    json_object_set_string_member(o, "inbox_agent", inbox_agent(self) != NULL ? inbox_agent(self) : "");
    return reply_object(request, o);
}

static JsonNode *
handle_peer_set(
    ClawtDaemon *self,
    JsonNode    *request,
    JsonObject  *payload
){
    JsonNode *error_reply = NULL;
    ClawtFedPeer *peer = peer_from_payload(self, payload, request, &error_reply);
    g_autoptr(GError) error = NULL;
    const gchar *tier_nick = clawt_ipc_payload_string(payload, "tier");
    const gchar *endpoint = clawt_ipc_payload_string(payload, "endpoint");
    const gchar *label = clawt_ipc_payload_string(payload, "label");
    const gchar *owner = clawt_ipc_payload_string(payload, "owner");
    const gchar *on_revoke = clawt_ipc_payload_string(payload, "on_revoke");
    JsonNode *grant_node = payload != NULL ? json_object_get_member(payload, "grant") : NULL;
    gboolean relink = FALSE;
    ClawtFedTier tier;

    if (peer == NULL)
        return error_reply;
    if (tier_nick != NULL) {
        if (!clawt_fed_tier_from_nick(tier_nick, &tier))
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "the tiers are owned, trusted, limited and untrusted");
        peer->tier = tier;
    }
    if (endpoint != NULL) {
        if (*endpoint == '\0') {
            g_clear_pointer(&peer->endpoint, g_free);
        } else {
            if (!clawt_fed_endpoint_check(endpoint, &error))
                return clawt_ipc_error_new(request, error->code, error->message);
            g_free(peer->endpoint);
            peer->endpoint = g_strdup(endpoint);
        }
        relink = TRUE;
    }
    if (label != NULL) {
        g_free(peer->label);
        peer->label = bounded(label, 64);
    }
    if (owner != NULL) {
        g_free(peer->owner);
        peer->owner = bounded(owner, 64);
    }
    if (on_revoke != NULL)
        peer->on_revoke = g_strcmp0(on_revoke, "finish") == 0 ? CLAWT_FED_ON_REVOKE_FINISH
                                                             : CLAWT_FED_ON_REVOKE_CANCEL;
    /* Grant overrides, checked whole against a scratch grant first. */
    if (grant_node != NULL && JSON_NODE_HOLDS_OBJECT(grant_node)) {
        JsonObject *changes = json_node_get_object(grant_node);
        g_autoptr(GList) members = json_object_get_members(changes);
        g_autoptr(ClawtFedGrant) scratch = clawt_fed_grant_preset(peer->tier);
        GList *l;

        for (l = members; l != NULL; l = l->next) {
            const gchar *value = obj_str(changes, l->data);
            if (value == NULL)
                return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                           "grant values are strings");
            if (*value != '\0' && !clawt_fed_grant_set(scratch, l->data, value, &error))
                return clawt_ipc_error_new(request, error->code, error->message);
        }
        if (peer->overrides == NULL)
            peer->overrides = json_object_new();
        for (l = members; l != NULL; l = l->next) {
            const gchar *value = obj_str(changes, l->data);
            g_autofree gchar *name = g_ascii_strdown(l->data, -1);
            /* An empty value removes the override, back to the tier and rules. */
            if (*value == '\0')
                json_object_remove_member(peer->overrides, name);
            else
                json_object_set_string_member(peer->overrides, name, value);
        }
    }
    save_registry(self);
    if (relink)
        clawt_daemon_federation_refresh_links(self);
    publish_peer(self, peer->alias, "changed");
    audit(self, "local", peer->alias, "peer.set", "ok", NULL);
    return handle_peer_show(self, request, payload);
}

static JsonNode *
handle_peer_tag(
    ClawtDaemon *self,
    JsonNode    *request,
    JsonObject  *payload
){
    JsonNode *error_reply = NULL;
    ClawtFedPeer *peer = peer_from_payload(self, payload, request, &error_reply);
    g_auto(GStrv) add = clawt_ipc_payload_strv(payload, "add");
    g_auto(GStrv) remove = clawt_ipc_payload_strv(payload, "remove");
    GPtrArray *tags;
    guint i;
    guint j;

    if (peer == NULL)
        return error_reply;
    for (i = 0; add != NULL && add[i] != NULL; i++)
        if (*add[i] == '\0' || strlen(add[i]) > 64 || strpbrk(add[i], ",+!: \t") != NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "a tag is 1 to 64 characters with no spaces or , + ! :");
    tags = g_ptr_array_new();
    for (i = 0; peer->tags != NULL && peer->tags[i] != NULL; i++) {
        gboolean drop = FALSE;
        for (j = 0; remove != NULL && remove[j] != NULL; j++)
            if (g_ascii_strcasecmp(peer->tags[i], remove[j]) == 0)
                drop = TRUE;
        if (!drop)
            g_ptr_array_add(tags, g_strdup(peer->tags[i]));
    }
    for (i = 0; add != NULL && add[i] != NULL; i++) {
        gboolean have = FALSE;
        for (j = 0; j < tags->len; j++)
            if (g_ascii_strcasecmp(g_ptr_array_index(tags, j), add[i]) == 0)
                have = TRUE;
        if (!have)
            g_ptr_array_add(tags, g_strdup(add[i]));
    }
    g_ptr_array_add(tags, NULL);
    g_strfreev(peer->tags);
    peer->tags = (gchar **)g_ptr_array_free(tags, FALSE);
    save_registry(self);
    publish_peer(self, peer->alias, "changed");
    return reply_object(request, peer_listing(self, peer));
}

static JsonNode *
handle_peer_flag(
    ClawtDaemon *self,
    JsonNode    *request,
    JsonObject  *payload,
    gboolean     muted
){
    JsonNode *error_reply = NULL;
    ClawtFedPeer *peer = peer_from_payload(self, payload, request, &error_reply);
    gboolean value = clawt_ipc_payload_boolean(payload, "value", TRUE);

    if (peer == NULL)
        return error_reply;
    if (muted)
        peer->muted = value;
    else
        peer->hidden = value;
    save_registry(self);
    publish_peer(self, peer->alias, "changed");
    return reply_object(request, peer_listing(self, peer));
}

typedef struct {
    ClawtDaemon *daemon;
    gchar       *alias;
} Notify;

static void
on_notified(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    Notify *n = user_data;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *body = clawt_fed_link_request_finish(CLAWT_FED_LINK(source), result, &error);

    if (n->daemon->fed_audit != NULL)
        audit(n->daemon, "out", n->alias, "notice", body != NULL ? "ok" : "failed",
              error != NULL ? error->message : NULL);
    g_object_unref(source);
    g_object_unref(n->daemon);
    g_free(n->alias);
    g_free(n);
}

/* Best effort: told while the link still exists, then forgotten. */
static void
notify_peer(
    ClawtDaemon  *self,
    ClawtFedPeer *peer,
    const gchar  *kind,
    const gchar  *body
){
    g_autoptr(GError) error = NULL;
    ClawtFedLink *link;
    Notify *n;

    if (peer->endpoint == NULL) {
        /* It dials us: say it down its own connection, if it has one up. */
        if (self->ipc_server != NULL) {
            g_autoptr(ClawtFedStamp) stamp = clawt_fed_stamp_sign(
                self->fed_key, self->fed_key_index, kind, self->fed_fleet_id, peer->fleet_id,
                body, "IPC fed.push", NULL);
            if (stamp != NULL) {
                g_autoptr(JsonNode) frame = NULL;
                JsonNode *payload = json_node_new(JSON_NODE_OBJECT);
                json_node_take_object(payload, clawt_fed_envelope_build(stamp, body));
                frame = clawt_ipc_request_new("fed.push", NULL);
                clawt_ipc_frame_set_payload(frame, payload);
                clawt_ipc_server_push_to_fleet(self->ipc_server, peer->fleet_id, frame);
            }
        }
        return;
    }
    link = make_link(self, peer, &error);
    if (link == NULL)
        return;
    n = g_new0(Notify, 1);
    n->daemon = g_object_ref(self);
    n->alias = g_strdup(peer->alias);
    clawt_fed_link_request_async(link, kind, body, NULL, on_notified, n);
}

static JsonNode *
handle_peer_revoke(
    ClawtDaemon *self,
    JsonNode    *request,
    JsonObject  *payload
){
    JsonNode *error_reply = NULL;
    ClawtFedPeer *peer = peer_from_payload(self, payload, request, &error_reply);

    if (peer == NULL)
        return error_reply;
    if (g_strcmp0(peer->status, "revoked") != 0)
        notify_peer(self, peer, "revoke", "{}");
    g_free(peer->status);
    peer->status = g_strdup("revoked");
    save_registry(self);
    clawt_daemon_federation_refresh_links(self);
    publish_peer(self, peer->alias, "revoked");
    audit(self, "local", peer->alias, "revoke", "ok", NULL);
    return reply_object(request, peer_listing(self, peer));
}

static JsonNode *
handle_peer_forget(
    ClawtDaemon *self,
    JsonNode    *request,
    JsonObject  *payload
){
    JsonNode *error_reply = NULL;
    ClawtFedPeer *peer = peer_from_payload(self, payload, request, &error_reply);
    g_autofree gchar *alias = NULL;
    g_autofree gchar *base = NULL;
    g_autofree gchar *access = NULL;

    if (peer == NULL)
        return error_reply;
    if (g_strcmp0(peer->status, "revoked") != 0)
        return clawt_ipc_error_new(request, CLAWT_ERROR_AGENT_STATE,
                                   "revoke the link first (clawtilla federation revoke ALIAS)");
    alias = g_strdup(peer->alias);
    base = secrets_path(self, alias);
    access = g_strconcat(base, ".cf-access", NULL);
    g_unlink(access);
    clawt_fed_registry_remove(self->fed_registry, alias);
    save_registry(self);
    clawt_daemon_federation_refresh_links(self);
    publish_peer(self, alias, "removed");
    return reply_object(request, json_object_new());
}

static JsonNode *
handle_peer_approve(
    ClawtDaemon *self,
    JsonNode    *request,
    JsonObject  *payload
){
    JsonNode *error_reply = NULL;
    ClawtFedPeer *peer = peer_from_payload(self, payload, request, &error_reply);

    if (peer == NULL)
        return error_reply;
    if (g_strcmp0(peer->status, "pending") != 0)
        return clawt_ipc_error_new(request, CLAWT_ERROR_AGENT_STATE,
                                   "that link is not waiting for approval");
    g_free(peer->status);
    peer->status = g_strdup("active");
    save_registry(self);
    clawt_daemon_federation_refresh_links(self);
    publish_peer(self, peer->alias, "changed");
    audit(self, "local", peer->alias, "approve", "ok", NULL);
    return reply_object(request, peer_listing(self, peer));
}

static JsonNode *
handle_peer_ping(
    ClawtDaemon *self,
    JsonNode    *request,
    JsonObject  *payload
){
    JsonNode *error_reply = NULL;
    ClawtFedPeer *peer = peer_from_payload(self, payload, request, &error_reply);
    PeerState *ps;
    ClawtIpcPending *pending;

    if (peer == NULL)
        return error_reply;
    ps = state_of(self, peer->alias);
    if (ps == NULL || ps->link == NULL)
        return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_CONNECTED,
                                   "this fleet has no endpoint here; it dials this one");
    pending = clawt_ipc_server_defer(self->ipc_server, request);
    if (pending == NULL)
        return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                   "this request cannot be answered later");
    ping_peer(self, ps, pending);
    return NULL;
}

static JsonNode *
handle_audit(
    ClawtDaemon *self,
    JsonNode    *request,
    JsonObject  *payload
){
    const gchar *alias = clawt_ipc_payload_string(payload, "alias");
    gint64 limit = clawt_ipc_payload_int(payload, "limit", 100);
    JsonObject *o = json_object_new();
    JsonArray *a = json_array_new();
    GList *l;
    gint64 n = 0;

    /* Newest first. */
    for (l = self->fed_audit->tail; l != NULL && n < limit; l = l->prev) {
        JsonObject *row = l->data;
        if (alias != NULL && g_strcmp0(obj_str(row, "peer"), alias) != 0)
            continue;
        json_array_add_object_element(a, json_object_ref(row));
        n++;
    }
    json_object_set_array_member(o, "entries", a);
    return reply_object(request, o);
}

static JsonNode *
handle_rotate(
    ClawtDaemon *self,
    JsonNode    *request
){
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtFleetKey) fresh = clawt_fleet_key_generate(&error);
    g_autoptr(GPtrArray) peers = NULL;
    g_autofree gchar *key_path = secrets_path(self, "fleet.ed25519");
    g_autofree gchar *archived = NULL;
    g_autofree gchar *pub = NULL;
    g_autofree gchar *seal = NULL;
    g_autofree gchar *proof_text = NULL;
    g_autofree gchar *proof = NULL;
    g_autoptr(JsonObject) body = json_object_new();
    g_autofree gchar *text = NULL;
    JsonObject *o;
    guint i;

    if (fresh == NULL)
        return clawt_ipc_error_new(request, error->code, error->message);
    pub = clawt_fleet_key_public_text(fresh);
    seal = clawt_fleet_key_seal_public_text(fresh);
    /* The new key signs that it belongs to this fleet... */
    proof_text = g_strdup_printf("rotate|%s|%s", self->fed_fleet_id, pub);
    proof = clawt_fleet_key_sign_text(fresh, proof_text);
    json_object_set_string_member(body, "key", pub);
    json_object_set_string_member(body, "seal_key", seal);
    json_object_set_string_member(body, "proof", proof);
    text = object_text(body);

    /* ... and the old key, which each peer still trusts, tells them. */
    peers = clawt_fed_registry_list(self->fed_registry);
    for (i = 0; i < peers->len; i++) {
        ClawtFedPeer *peer = g_ptr_array_index(peers, i);
        if (g_strcmp0(peer->status, "revoked") != 0)
            notify_peer(self, peer, "rotate", text);
    }

    /* The old seed is kept, numbered, beside the new one. */
    archived = g_strdup_printf("%s.%u", key_path, self->fed_key_index);
    if (g_rename(key_path, archived) != 0)
        return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED, "cannot archive the old key");
    if (!clawt_fleet_key_save(fresh, key_path, &error)) {
        g_rename(archived, key_path);
        return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED, error->message);
    }
    clawt_fleet_key_unref(self->fed_key);
    self->fed_key = clawt_fleet_key_ref(fresh);
    self->fed_key_index++;
    save_identity(self, NULL);
    clawt_daemon_federation_refresh_links(self);
    audit(self, "local", NULL, "rotate", "ok", NULL);

    o = json_object_new();
    json_object_set_int_member(o, "key_index", self->fed_key_index);
    json_object_set_string_member(o, "fleet_id", self->fed_fleet_id);
    json_object_set_int_member(o, "told", peers->len);
    return reply_object(request, o);
}

static JsonNode *
handle_expose(
    ClawtDaemon *self,
    JsonNode    *request,
    JsonObject  *payload
){
    const gchar *hostname = clawt_ipc_payload_string(payload, "hostname");
    g_autofree gchar *sock = g_build_filename(self->state_dir, "federation.sock", NULL);
    g_autofree gchar *config = NULL;
    g_autofree gchar *url = NULL;
    JsonObject *o;

    if (hostname == NULL || *hostname == '\0')
        hostname = "fleet.example.com";
    url = g_strdup_printf("https://%s", hostname);
    config = g_strdup_printf(
        "# ~/.cloudflared/config.yml -- then: cloudflared tunnel run\n"
        "tunnel: <TUNNEL-ID>\n"
        "credentials-file: ~/.cloudflared/<TUNNEL-ID>.json\n"
        "ingress:\n"
        "  - hostname: %s\n"
        "    path: ^/fed/v1/\n"
        "    service: unix:%s\n"
        "  - service: http_status:404\n", hostname, sock);
    o = json_object_new();
    json_object_set_string_member(o, "cloudflared_config", config);
    json_object_set_string_member(o, "public_url", url);
    json_object_set_string_member(o, "socket", sock);
    json_object_set_string_member(o, "next",
        "cloudflared tunnel create NAME; cloudflared tunnel route dns NAME HOSTNAME; "
        "put the config above in place; clawtilla config set federation.public_url URL; "
        "cloudflared tunnel run NAME");
    return reply_object(request, o);
}

JsonNode *
clawt_daemon_handle_federation(
    ClawtDaemon *self,
    const gchar *kind,
    JsonNode    *request,
    JsonObject  *payload,
    gboolean    *handled
){
    if (kind == NULL || !g_str_has_prefix(kind, "federation.")) {
        *handled = FALSE;
        return NULL;
    }
    *handled = TRUE;

    if (g_strcmp0(kind, "federation.status") == 0)
        return handle_status(self, request);
    if (g_strcmp0(kind, "federation.tiers") == 0) {
        JsonObject *o = json_object_new();
        JsonArray *a = json_array_new();
        guint i;
        for (i = 0; i < clawt_fed_tier_count(); i++) {
            ClawtFedTier t = clawt_fed_tier_nth(i);
            g_autoptr(ClawtFedGrant) g = clawt_fed_grant_preset(t);
            JsonObject *e = json_object_new();
            json_object_set_string_member(e, "tier", clawt_fed_tier_nick(t));
            json_object_set_string_member(e, "label", clawt_fed_tier_label(t));
            json_object_set_string_member(e, "summary", clawt_fed_tier_summary(t));
            json_object_set_string_member(e, "tone", clawt_fed_tier_tone(t));
            json_object_set_object_member(e, "grant", clawt_fed_grant_to_json(g));
            json_array_add_object_element(a, e);
        }
        json_object_set_array_member(o, "tiers", a);
        return reply_object(request, o);
    }
    if (g_strcmp0(kind, "federation.invite.inspect") == 0) {
        g_autoptr(GError) error = NULL;
        g_autoptr(ClawtFedInvite) invite =
            clawt_fed_invite_parse(clawt_ipc_payload_string(payload, "invite"), &error);
        if (invite == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);
        return reply_object(request, invite_preview(self, invite));
    }
    if (g_strcmp0(kind, "federation.expose") == 0)
        return handle_expose(self, request, payload);

    if (!fed_on(self))
        return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
                                   "federation is off: set federation.enabled: true and "
                                   "restart the daemon");

    if (g_strcmp0(kind, "federation.invite.create") == 0
        || g_strcmp0(kind, "federation.link.offer") == 0)
        return handle_invite_create(self, request, payload);
    if (g_strcmp0(kind, "federation.invite.accept") == 0)
        return handle_invite_accept(self, request, payload);
    if (g_strcmp0(kind, "federation.invite.revoke") == 0) {
        if (!clawt_fed_registry_remove_invite(self->fed_registry,
                                              clawt_ipc_payload_string(payload, "id")))
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND, "no such invite");
        save_registry(self);
        return reply_object(request, json_object_new());
    }
    if (g_strcmp0(kind, "federation.link") == 0)
        return handle_link(self, request, payload);
    if (g_strcmp0(kind, "federation.peer.list") == 0)
        return handle_peer_list(self, request, payload);
    if (g_strcmp0(kind, "federation.peer.show") == 0
        || g_strcmp0(kind, "federation.peer.explain") == 0)
        return handle_peer_show(self, request, payload);
    if (g_strcmp0(kind, "federation.peer.set") == 0)
        return handle_peer_set(self, request, payload);
    if (g_strcmp0(kind, "federation.peer.tag") == 0)
        return handle_peer_tag(self, request, payload);
    if (g_strcmp0(kind, "federation.peer.hide") == 0)
        return handle_peer_flag(self, request, payload, FALSE);
    if (g_strcmp0(kind, "federation.peer.mute") == 0)
        return handle_peer_flag(self, request, payload, TRUE);
    if (g_strcmp0(kind, "federation.peer.approve") == 0)
        return handle_peer_approve(self, request, payload);
    if (g_strcmp0(kind, "federation.peer.revoke") == 0)
        return handle_peer_revoke(self, request, payload);
    if (g_strcmp0(kind, "federation.peer.forget") == 0)
        return handle_peer_forget(self, request, payload);
    if (g_strcmp0(kind, "federation.peer.ping") == 0)
        return handle_peer_ping(self, request, payload);
    if (g_strcmp0(kind, "federation.audit") == 0)
        return handle_audit(self, request, payload);
    if (g_strcmp0(kind, "federation.rotate_key") == 0)
        return handle_rotate(self, request);

    {
        g_autofree gchar *message = g_strdup_printf("this daemon does not understand '%s'", kind);
        return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED, message);
    }
}
