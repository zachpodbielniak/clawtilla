/*
 * clawt-fed-registry.c - The fleets this one is linked with
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "federation/clawt-fed-registry.h"
#include <yaml-glib.h>

#include <errno.h>
#include <string.h>

/* ---- tolerant readers --------------------------------------------------
 * The file goes YAML -> JSON, and a YAML scalar may arrive as a string
 * where JSON would have had a number or a boolean.  Read both, so a
 * person who edits `hidden: yes` by hand is not silently ignored.
 */

static const gchar *
read_str(
    JsonObject  *o,
    const gchar *name
){
    JsonNode *n = o != NULL ? json_object_get_member(o, name) : NULL;

    if (n == NULL || !JSON_NODE_HOLDS_VALUE(n)
        || json_node_get_value_type(n) != G_TYPE_STRING)
        return NULL;
    return json_node_get_string(n);
}

static gint64
read_int(
    JsonObject  *o,
    const gchar *name,
    gint64       fallback
){
    JsonNode *n = o != NULL ? json_object_get_member(o, name) : NULL;
    GType t;

    if (n == NULL || !JSON_NODE_HOLDS_VALUE(n))
        return fallback;
    t = json_node_get_value_type(n);
    if (t == G_TYPE_INT64)
        return json_node_get_int(n);
    if (t == G_TYPE_DOUBLE)
        return (gint64)json_node_get_double(n);
    if (t == G_TYPE_STRING) {
        const gchar *s = json_node_get_string(n);
        gchar *end = NULL;
        gint64 v = g_ascii_strtoll(s, &end, 10);
        if (end != s && *end == '\0')
            return v;
    }
    return fallback;
}

static gboolean
read_bool(
    JsonObject  *o,
    const gchar *name
){
    JsonNode *n = o != NULL ? json_object_get_member(o, name) : NULL;
    GType t;

    if (n == NULL || !JSON_NODE_HOLDS_VALUE(n))
        return FALSE;
    t = json_node_get_value_type(n);
    if (t == G_TYPE_BOOLEAN)
        return json_node_get_boolean(n);
    if (t == G_TYPE_STRING) {
        const gchar *s = json_node_get_string(n);
        return g_ascii_strcasecmp(s, "true") == 0 || g_ascii_strcasecmp(s, "yes") == 0
            || g_ascii_strcasecmp(s, "on") == 0;
    }
    if (t == G_TYPE_INT64)
        return json_node_get_int(n) != 0;
    return FALSE;
}

static gchar **
read_strv(
    JsonObject  *o,
    const gchar *name
){
    JsonNode *n = o != NULL ? json_object_get_member(o, name) : NULL;
    GPtrArray *out = g_ptr_array_new();
    JsonArray *a;
    guint i;

    if (n != NULL && JSON_NODE_HOLDS_ARRAY(n) && (a = json_node_get_array(n)) != NULL) {
        for (i = 0; i < json_array_get_length(a); i++) {
            JsonNode *item = json_array_get_element(a, i);
            if (JSON_NODE_HOLDS_VALUE(item)
                && json_node_get_value_type(item) == G_TYPE_STRING)
                g_ptr_array_add(out, g_strdup(json_node_get_string(item)));
        }
    }
    g_ptr_array_add(out, NULL);
    return (gchar **)g_ptr_array_free(out, FALSE);
}

static JsonObject *
read_object(
    JsonObject  *o,
    const gchar *name
){
    JsonNode *n = o != NULL ? json_object_get_member(o, name) : NULL;

    if (n == NULL || !JSON_NODE_HOLDS_OBJECT(n) || json_node_get_object(n) == NULL)
        return NULL;
    return json_object_ref(json_node_get_object(n));
}

/* An object whose every value is a string: YAML may have typed them. */
static JsonObject *
stringify_object(
    JsonObject *o
){
    JsonObject *out;
    g_autoptr(GList) members = NULL;
    GList *l;

    if (o == NULL)
        return NULL;
    out = json_object_new();
    members = json_object_get_members(o);
    for (l = members; l != NULL; l = l->next) {
        JsonNode *n = json_object_get_member(o, l->data);
        if (!JSON_NODE_HOLDS_VALUE(n))
            continue;
        switch (json_node_get_value_type(n)) {
        case G_TYPE_STRING:
            json_object_set_string_member(out, l->data, json_node_get_string(n));
            break;
        case G_TYPE_BOOLEAN:
            json_object_set_string_member(out, l->data,
                                          json_node_get_boolean(n) ? "true" : "false");
            break;
        case G_TYPE_INT64: {
            g_autofree gchar *v = g_strdup_printf("%" G_GINT64_FORMAT, json_node_get_int(n));
            json_object_set_string_member(out, l->data, v);
            break;
        }
        default:
            break;
        }
    }
    return out;
}

/* ---- peers ------------------------------------------------------------ */

G_DEFINE_BOXED_TYPE(ClawtFedPeer, clawt_fed_peer, clawt_fed_peer_copy, clawt_fed_peer_free)

ClawtFedPeer *
clawt_fed_peer_new(
    const gchar *alias,
    const gchar *fleet_id,
    const gchar *public_key
){
    ClawtFedPeer *peer;
    gint64 zero = 0;

    peer = g_new0(ClawtFedPeer, 1);
    peer->alias = g_strdup(alias);
    peer->fleet_id = g_strdup(fleet_id);
    peer->keys = g_ptr_array_new_with_free_func(g_free);
    peer->key_until = g_array_new(FALSE, TRUE, sizeof(gint64));
    if (public_key != NULL) {
        g_ptr_array_add(peer->keys, g_strdup(public_key));
        g_array_append_val(peer->key_until, zero);
    }
    peer->tier = CLAWT_FED_TIER_UNTRUSTED;
    peer->tags = g_new0(gchar *, 1);
    peer->status = g_strdup("active");
    peer->created_at = g_get_real_time() / G_USEC_PER_SEC;
    return peer;
}

ClawtFedPeer *
clawt_fed_peer_copy(
    const ClawtFedPeer *peer
){
    ClawtFedPeer *copy;
    guint i;

    copy = g_new0(ClawtFedPeer, 1);
    copy->alias = g_strdup(peer->alias);
    copy->label = g_strdup(peer->label);
    copy->fleet_id = g_strdup(peer->fleet_id);
    copy->keys = g_ptr_array_new_with_free_func(g_free);
    for (i = 0; i < peer->keys->len; i++)
        g_ptr_array_add(copy->keys, g_strdup(g_ptr_array_index(peer->keys, i)));
    copy->key_until = g_array_copy(peer->key_until);
    copy->seal_key = g_strdup(peer->seal_key);
    copy->endpoint = g_strdup(peer->endpoint);
    copy->tier = peer->tier;
    copy->owner = g_strdup(peer->owner);
    copy->tags = g_strdupv(peer->tags);
    copy->overrides = peer->overrides != NULL ? json_object_ref(peer->overrides) : NULL;
    copy->hidden = peer->hidden;
    copy->muted = peer->muted;
    copy->on_revoke = peer->on_revoke;
    copy->status = g_strdup(peer->status);
    copy->created_at = peer->created_at;
    copy->their_grant = peer->their_grant != NULL ? json_object_ref(peer->their_grant) : NULL;
    copy->cert_pin = g_strdup(peer->cert_pin);
    copy->version = g_strdup(peer->version);
    copy->access_id = g_strdup(peer->access_id);
    return copy;
}

void
clawt_fed_peer_free(
    ClawtFedPeer *peer
){
    if (peer == NULL)
        return;
    g_free(peer->alias);
    g_free(peer->label);
    g_free(peer->fleet_id);
    g_ptr_array_unref(peer->keys);
    g_array_unref(peer->key_until);
    g_free(peer->seal_key);
    g_free(peer->endpoint);
    g_free(peer->owner);
    g_strfreev(peer->tags);
    if (peer->overrides != NULL)
        json_object_unref(peer->overrides);
    g_free(peer->status);
    if (peer->their_grant != NULL)
        json_object_unref(peer->their_grant);
    g_free(peer->cert_pin);
    g_free(peer->version);
    g_free(peer->access_id);
    g_free(peer);
}

gboolean
clawt_fed_peer_public_key(
    const ClawtFedPeer *peer,
    guint               index,
    gint64              now,
    guint8             *out_key
){
    gint64 until;

    if (index >= peer->keys->len)
        return FALSE;
    until = g_array_index(peer->key_until, gint64, index);
    if (until != 0 && now > until)
        return FALSE;
    return clawt_fed_decode_key(g_ptr_array_index(peer->keys, index), out_key);
}

guint
clawt_fed_peer_rotate(
    ClawtFedPeer *peer,
    const gchar  *new_key,
    gint64        now
){
    gint64 zero = 0;
    guint i;

    for (i = 0; i < peer->key_until->len; i++)
        if (g_array_index(peer->key_until, gint64, i) == 0)
            g_array_index(peer->key_until, gint64, i) = now + CLAWT_FED_KEY_GRACE_SECONDS;
    g_ptr_array_add(peer->keys, g_strdup(new_key));
    g_array_append_val(peer->key_until, zero);
    return peer->keys->len - 1;
}

const gchar *
clawt_fed_peer_display(
    const ClawtFedPeer *peer
){
    return peer->label != NULL && *peer->label != '\0' ? peer->label : peer->alias;
}

JsonObject *
clawt_fed_peer_selector_json(
    const ClawtFedPeer *peer,
    ClawtFedHealth      health
){
    JsonObject *o;
    JsonArray *tags;
    guint i;

    o = json_object_new();
    json_object_set_string_member(o, "alias", peer->alias);
    json_object_set_string_member(o, "label", clawt_fed_peer_display(peer));
    json_object_set_string_member(o, "fleet_id", peer->fleet_id);
    json_object_set_string_member(o, "tier", clawt_fed_tier_nick(peer->tier));
    json_object_set_string_member(o, "tier_tone", clawt_fed_tier_tone(peer->tier));
    if (peer->owner != NULL)
        json_object_set_string_member(o, "owner", peer->owner);
    tags = json_array_new();
    for (i = 0; peer->tags != NULL && peer->tags[i] != NULL; i++)
        json_array_add_string_element(tags, peer->tags[i]);
    json_object_set_array_member(o, "tags", tags);
    json_object_set_string_member(o, "health", clawt_fed_health_nick(health));
    json_object_set_string_member(o, "health_tone", clawt_fed_health_tone(health));
    json_object_set_boolean_member(o, "hidden", peer->hidden);
    json_object_set_boolean_member(o, "muted", peer->muted);
    json_object_set_string_member(o, "status", peer->status != NULL ? peer->status : "active");
    if (peer->endpoint != NULL)
        json_object_set_string_member(o, "endpoint", peer->endpoint);
    if (peer->version != NULL)
        json_object_set_string_member(o, "version", peer->version);
    return o;
}

gboolean
clawt_fed_alias_check(
    const gchar  *alias,
    GError      **error
){
    if (alias == NULL || !clawt_is_valid_id(alias) || strlen(alias) > CLAWT_FED_ALIAS_MAX) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "\"%s\" is not a usable alias: lowercase letters, digits, "
                    "- and _, at most %d characters",
                    alias != NULL ? alias : "", CLAWT_FED_ALIAS_MAX);
        return FALSE;
    }
    return TRUE;
}

gchar *
clawt_fed_alias_suggest(
    const gchar *name
){
    GString *out;
    const gchar *p;

    out = g_string_new(NULL);
    for (p = name; p != NULL && *p != '\0' && out->len < CLAWT_FED_ALIAS_MAX; p++) {
        gchar c = g_ascii_tolower(*p);
        if (g_ascii_isalnum(c))
            g_string_append_c(out, c);
        else if (out->len > 0 && out->str[out->len - 1] != '-')
            g_string_append_c(out, '-');
    }
    while (out->len > 0 && out->str[out->len - 1] == '-')
        g_string_truncate(out, out->len - 1);
    if (out->len == 0 || !g_ascii_isalnum(out->str[0]))
        g_string_assign(out, "peer");
    return g_string_free(out, FALSE);
}

static JsonObject *
peer_to_json(
    const ClawtFedPeer *peer
){
    JsonObject *o;
    JsonArray *keys;
    JsonArray *tags;
    guint i;

    o = json_object_new();
    json_object_set_string_member(o, "alias", peer->alias);
    if (peer->label != NULL)
        json_object_set_string_member(o, "label", peer->label);
    json_object_set_string_member(o, "fleet_id", peer->fleet_id);
    keys = json_array_new();
    for (i = 0; i < peer->keys->len; i++) {
        JsonObject *k = json_object_new();
        json_object_set_string_member(k, "key", g_ptr_array_index(peer->keys, i));
        json_object_set_int_member(k, "until", g_array_index(peer->key_until, gint64, i));
        json_array_add_object_element(keys, k);
    }
    json_object_set_array_member(o, "keys", keys);
    if (peer->seal_key != NULL)
        json_object_set_string_member(o, "seal_key", peer->seal_key);
    if (peer->endpoint != NULL)
        json_object_set_string_member(o, "endpoint", peer->endpoint);
    json_object_set_string_member(o, "tier", clawt_fed_tier_nick(peer->tier));
    if (peer->owner != NULL)
        json_object_set_string_member(o, "owner", peer->owner);
    tags = json_array_new();
    for (i = 0; peer->tags != NULL && peer->tags[i] != NULL; i++)
        json_array_add_string_element(tags, peer->tags[i]);
    json_object_set_array_member(o, "tags", tags);
    if (peer->overrides != NULL && json_object_get_size(peer->overrides) > 0)
        json_object_set_object_member(o, "overrides", json_object_ref(peer->overrides));
    json_object_set_boolean_member(o, "hidden", peer->hidden);
    json_object_set_boolean_member(o, "muted", peer->muted);
    json_object_set_string_member(o, "on_revoke",
                                  peer->on_revoke == CLAWT_FED_ON_REVOKE_FINISH ? "finish" : "cancel");
    json_object_set_string_member(o, "status", peer->status != NULL ? peer->status : "active");
    json_object_set_int_member(o, "created_at", peer->created_at);
    if (peer->their_grant != NULL)
        json_object_set_object_member(o, "their_grant", json_object_ref(peer->their_grant));
    if (peer->cert_pin != NULL)
        json_object_set_string_member(o, "cert_pin", peer->cert_pin);
    if (peer->version != NULL)
        json_object_set_string_member(o, "version", peer->version);
    if (peer->access_id != NULL)
        json_object_set_string_member(o, "access_id", peer->access_id);
    return o;
}

static ClawtFedPeer *
peer_from_json(
    JsonObject  *o,
    GError     **error
){
    g_autoptr(ClawtFedPeer) peer = NULL;
    JsonNode *keys_node;
    const gchar *alias;
    const gchar *fleet;
    const gchar *status;
    guint i;

    alias = read_str(o, "alias");
    fleet = read_str(o, "fleet_id");
    if (!clawt_fed_alias_check(alias, error))
        return NULL;
    if (fleet == NULL || strlen(fleet) != CLAWT_FLEET_FINGERPRINT_LENGTH) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "peer %s has no fleet_id", alias);
        return NULL;
    }
    peer = clawt_fed_peer_new(alias, fleet, NULL);

    keys_node = json_object_get_member(o, "keys");
    if (keys_node != NULL && JSON_NODE_HOLDS_ARRAY(keys_node)) {
        JsonArray *keys = json_node_get_array(keys_node);
        for (i = 0; keys != NULL && i < json_array_get_length(keys); i++) {
            JsonNode *item = json_array_get_element(keys, i);
            JsonObject *k = JSON_NODE_HOLDS_OBJECT(item) ? json_node_get_object(item) : NULL;
            const gchar *key = read_str(k, "key");
            guint8 raw[CLAWT_FLEET_KEY_SIZE];
            gint64 until = read_int(k, "until", 0);
            if (key == NULL || !clawt_fed_decode_key(key, raw)) {
                g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "peer %s has an unreadable key at index %u", alias, i);
                return NULL;
            }
            g_ptr_array_add(peer->keys, g_strdup(key));
            g_array_append_val(peer->key_until, until);
        }
    }
    if (peer->keys->len == 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "peer %s has no keys", alias);
        return NULL;
    }

    peer->label = g_strdup(read_str(o, "label"));
    peer->seal_key = g_strdup(read_str(o, "seal_key"));
    peer->endpoint = g_strdup(read_str(o, "endpoint"));
    if (!clawt_fed_tier_from_nick(read_str(o, "tier"), &peer->tier))
        peer->tier = CLAWT_FED_TIER_UNTRUSTED;
    peer->owner = g_strdup(read_str(o, "owner"));
    g_strfreev(peer->tags);
    peer->tags = read_strv(o, "tags");
    {
        g_autoptr(JsonObject) raw = read_object(o, "overrides");
        peer->overrides = stringify_object(raw);
    }
    peer->hidden = read_bool(o, "hidden");
    peer->muted = read_bool(o, "muted");
    peer->on_revoke = g_strcmp0(read_str(o, "on_revoke"), "finish") == 0
        ? CLAWT_FED_ON_REVOKE_FINISH : CLAWT_FED_ON_REVOKE_CANCEL;
    status = read_str(o, "status");
    g_free(peer->status);
    peer->status = g_strdup(g_strcmp0(status, "pending") == 0 || g_strcmp0(status, "revoked") == 0
                            ? status : "active");
    peer->created_at = read_int(o, "created_at", 0);
    peer->their_grant = read_object(o, "their_grant");
    peer->cert_pin = g_strdup(read_str(o, "cert_pin"));
    peer->version = g_strdup(read_str(o, "version"));
    peer->access_id = g_strdup(read_str(o, "access_id"));
    return g_steal_pointer(&peer);
}

/* ---- pending invites --------------------------------------------------- */

G_DEFINE_BOXED_TYPE(ClawtFedPendingInvite, clawt_fed_pending_invite,
                    clawt_fed_pending_invite_copy, clawt_fed_pending_invite_free)

ClawtFedPendingInvite *
clawt_fed_pending_invite_copy(
    const ClawtFedPendingInvite *invite
){
    ClawtFedPendingInvite *copy;

    copy = g_new0(ClawtFedPendingInvite, 1);
    *copy = *invite;
    copy->id = g_strdup(invite->id);
    copy->secret = g_strdup(invite->secret);
    copy->note = g_strdup(invite->note);
    return copy;
}

void
clawt_fed_pending_invite_free(
    ClawtFedPendingInvite *invite
){
    if (invite == NULL)
        return;
    g_free(invite->id);
    if (invite->secret != NULL)
        memset(invite->secret, 0, strlen(invite->secret));
    g_free(invite->secret);
    g_free(invite->note);
    g_free(invite);
}

/* ---- the registry ------------------------------------------------------ */

struct _ClawtFedRegistry {
    GObject     parent_instance;
    gchar      *peers_path;
    gchar      *invites_path;
    GHashTable *peers;     /* alias -> ClawtFedPeer */
    GHashTable *invites;   /* id -> ClawtFedPendingInvite */
};

G_DEFINE_FINAL_TYPE(ClawtFedRegistry, clawt_fed_registry, G_TYPE_OBJECT)

static void
clawt_fed_registry_finalize(
    GObject *object
){
    ClawtFedRegistry *self = CLAWT_FED_REGISTRY(object);

    g_free(self->peers_path);
    g_free(self->invites_path);
    g_hash_table_unref(self->peers);
    g_hash_table_unref(self->invites);
    G_OBJECT_CLASS(clawt_fed_registry_parent_class)->finalize(object);
}

static void
clawt_fed_registry_class_init(
    ClawtFedRegistryClass *klass
){
    G_OBJECT_CLASS(klass)->finalize = clawt_fed_registry_finalize;
}

static void
clawt_fed_registry_init(
    ClawtFedRegistry *self
){
    self->peers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                        (GDestroyNotify)clawt_fed_peer_free);
    self->invites = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                          (GDestroyNotify)clawt_fed_pending_invite_free);
}

ClawtFedRegistry *
clawt_fed_registry_new(
    const gchar *peers_path,
    const gchar *invites_path
){
    ClawtFedRegistry *self;

    self = g_object_new(CLAWT_TYPE_FED_REGISTRY, NULL);
    self->peers_path = g_strdup(peers_path);
    self->invites_path = g_strdup(invites_path);
    return self;
}

/* The top-level object of a YAML file; NULL for a missing file. */
static JsonObject *
load_yaml_object(
    const gchar  *path,
    gboolean     *out_missing,
    GError      **error
){
    g_autoptr(YamlParser) parser = NULL;
    g_autoptr(JsonNode) root = NULL;
    YamlNode *node;

    *out_missing = FALSE;
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        *out_missing = TRUE;
        return NULL;
    }
    parser = yaml_parser_new();
    if (!yaml_parser_load_from_file(parser, path, error))
        return NULL;
    node = yaml_parser_get_root(parser);
    root = node != NULL ? yaml_node_to_json_node(node) : NULL;
    if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root) || json_node_get_object(root) == NULL)
        return json_object_new();
    return json_object_ref(json_node_get_object(root));
}

static gboolean
save_yaml_object(
    const gchar  *path,
    JsonObject   *object,
    GError      **error
){
    g_autoptr(JsonNode) root = json_node_new(JSON_NODE_OBJECT);
    g_autoptr(YamlNode) yaml = NULL;
    g_autoptr(YamlGenerator) generator = NULL;
    g_autofree gchar *text = NULL;
    g_autofree gchar *dir = NULL;

    json_node_set_object(root, object);
    yaml = yaml_node_from_json_node(root);
    generator = yaml_generator_new();
    yaml_generator_set_root(generator, yaml);
    text = yaml_generator_to_data(generator, NULL, error);
    if (text == NULL)
        return FALSE;
    dir = g_path_get_dirname(path);
    if (!clawt_ensure_dir(dir, 0700, error))
        return FALSE;
    return clawt_write_file_atomic(path, text, -1, 0600, FALSE, error);
}

gboolean
clawt_fed_registry_load(
    ClawtFedRegistry  *self,
    GError           **error
){
    g_autoptr(JsonObject) peers_doc = NULL;
    g_autoptr(JsonObject) invites_doc = NULL;
    JsonNode *list;
    gboolean missing;
    gboolean set_aside = FALSE;
    guint i;

    g_hash_table_remove_all(self->peers);
    g_hash_table_remove_all(self->invites);

    peers_doc = load_yaml_object(self->peers_path, &missing, error);
    if (peers_doc == NULL && !missing)
        return FALSE;
    list = peers_doc != NULL ? json_object_get_member(peers_doc, "peers") : NULL;
    if (list != NULL && JSON_NODE_HOLDS_ARRAY(list)) {
        JsonArray *a = json_node_get_array(list);
        for (i = 0; a != NULL && i < json_array_get_length(a); i++) {
            g_autoptr(GError) local = NULL;
            JsonNode *item = json_array_get_element(a, i);
            ClawtFedPeer *peer = NULL;

            if (JSON_NODE_HOLDS_OBJECT(item))
                peer = peer_from_json(json_node_get_object(item), &local);
            if (peer == NULL || g_hash_table_contains(self->peers, peer->alias)) {
                g_warning("federation: peer %u in %s is unreadable (%s); "
                          "a copy of the file is kept as %s.unreadable",
                          i, self->peers_path,
                          local != NULL ? local->message : "duplicate alias",
                          self->peers_path);
                set_aside = TRUE;
                clawt_fed_peer_free(peer);
                continue;
            }
            g_hash_table_insert(self->peers, g_strdup(peer->alias), peer);
        }
    }
    if (set_aside) {
        g_autofree gchar *copy = g_strconcat(self->peers_path, ".unreadable", NULL);
        g_autofree gchar *contents = NULL;
        gsize length;
        if (g_file_get_contents(self->peers_path, &contents, &length, NULL))
            clawt_write_file_atomic(copy, contents, length, 0600, FALSE, NULL);
    }

    invites_doc = load_yaml_object(self->invites_path, &missing, NULL);
    list = invites_doc != NULL ? json_object_get_member(invites_doc, "invites") : NULL;
    if (list != NULL && JSON_NODE_HOLDS_ARRAY(list)) {
        JsonArray *a = json_node_get_array(list);
        for (i = 0; a != NULL && i < json_array_get_length(a); i++) {
            JsonNode *item = json_array_get_element(a, i);
            JsonObject *o = JSON_NODE_HOLDS_OBJECT(item) ? json_node_get_object(item) : NULL;
            ClawtFedPendingInvite *inv;

            if (read_str(o, "id") == NULL || read_str(o, "secret") == NULL)
                continue;
            inv = g_new0(ClawtFedPendingInvite, 1);
            inv->id = g_strdup(read_str(o, "id"));
            inv->secret = g_strdup(read_str(o, "secret"));
            if (!clawt_fed_tier_from_nick(read_str(o, "tier"), &inv->tier))
                inv->tier = CLAWT_FED_TIER_UNTRUSTED;
            inv->expires = read_int(o, "expires", 0);
            inv->confirm = read_bool(o, "confirm");
            inv->note = g_strdup(read_str(o, "note"));
            inv->created_at = read_int(o, "created_at", 0);
            g_hash_table_replace(self->invites, g_strdup(inv->id), inv);
        }
    }
    return TRUE;
}

static gint
by_alias(
    gconstpointer a,
    gconstpointer b
){
    const ClawtFedPeer *pa = *(ClawtFedPeer * const *)a;
    const ClawtFedPeer *pb = *(ClawtFedPeer * const *)b;

    return g_strcmp0(pa->alias, pb->alias);
}

gboolean
clawt_fed_registry_save(
    ClawtFedRegistry  *self,
    GError           **error
){
    g_autoptr(GPtrArray) peers = clawt_fed_registry_list(self);
    g_autoptr(JsonObject) doc = json_object_new();
    g_autoptr(JsonObject) invites_doc = json_object_new();
    JsonArray *list = json_array_new();
    JsonArray *invites = json_array_new();
    GHashTableIter iter;
    gpointer value;
    guint i;

    for (i = 0; i < peers->len; i++)
        json_array_add_object_element(list, peer_to_json(g_ptr_array_index(peers, i)));
    json_object_set_array_member(doc, "peers", list);
    if (!save_yaml_object(self->peers_path, doc, error)) {
        json_array_unref(invites);
        return FALSE;
    }

    g_hash_table_iter_init(&iter, self->invites);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        ClawtFedPendingInvite *inv = value;
        JsonObject *o = json_object_new();
        json_object_set_string_member(o, "id", inv->id);
        json_object_set_string_member(o, "secret", inv->secret);
        json_object_set_string_member(o, "tier", clawt_fed_tier_nick(inv->tier));
        json_object_set_int_member(o, "expires", inv->expires);
        json_object_set_boolean_member(o, "confirm", inv->confirm);
        if (inv->note != NULL)
            json_object_set_string_member(o, "note", inv->note);
        json_object_set_int_member(o, "created_at", inv->created_at);
        json_array_add_object_element(invites, o);
    }
    json_object_set_array_member(invites_doc, "invites", invites);
    return save_yaml_object(self->invites_path, invites_doc, error);
}

GPtrArray *
clawt_fed_registry_list(
    ClawtFedRegistry *self
){
    GPtrArray *out;
    GHashTableIter iter;
    gpointer value;

    out = g_ptr_array_new();
    g_hash_table_iter_init(&iter, self->peers);
    while (g_hash_table_iter_next(&iter, NULL, &value))
        g_ptr_array_add(out, value);
    g_ptr_array_sort(out, by_alias);
    return out;
}

ClawtFedPeer *
clawt_fed_registry_get(
    ClawtFedRegistry *self,
    const gchar      *alias
){
    return alias != NULL ? g_hash_table_lookup(self->peers, alias) : NULL;
}

ClawtFedPeer *
clawt_fed_registry_by_fleet(
    ClawtFedRegistry *self,
    const gchar      *fleet_id
){
    GHashTableIter iter;
    gpointer value;

    if (fleet_id == NULL)
        return NULL;
    g_hash_table_iter_init(&iter, self->peers);
    while (g_hash_table_iter_next(&iter, NULL, &value))
        if (g_strcmp0(((ClawtFedPeer *)value)->fleet_id, fleet_id) == 0)
            return value;
    return NULL;
}

gboolean
clawt_fed_registry_add(
    ClawtFedRegistry  *self,
    ClawtFedPeer      *peer,
    GError           **error
){
    ClawtFedPeer *existing;

    if (!clawt_fed_alias_check(peer->alias, error)) {
        clawt_fed_peer_free(peer);
        return FALSE;
    }
    if (g_hash_table_contains(self->peers, peer->alias)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_ALREADY_EXISTS,
                    "a peer is already called %s; choose another alias", peer->alias);
        clawt_fed_peer_free(peer);
        return FALSE;
    }
    existing = clawt_fed_registry_by_fleet(self, peer->fleet_id);
    if (existing != NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_ALREADY_EXISTS,
                    "that fleet is already linked as %s", existing->alias);
        clawt_fed_peer_free(peer);
        return FALSE;
    }
    g_hash_table_insert(self->peers, g_strdup(peer->alias), peer);
    return TRUE;
}

gboolean
clawt_fed_registry_remove(
    ClawtFedRegistry *self,
    const gchar      *alias
){
    return g_hash_table_remove(self->peers, alias);
}

void
clawt_fed_registry_add_invite(
    ClawtFedRegistry      *self,
    ClawtFedPendingInvite *invite
){
    g_hash_table_replace(self->invites, g_strdup(invite->id), invite);
}

ClawtFedPendingInvite *
clawt_fed_registry_find_invite(
    ClawtFedRegistry *self,
    const gchar      *id
){
    return id != NULL ? g_hash_table_lookup(self->invites, id) : NULL;
}

gboolean
clawt_fed_registry_remove_invite(
    ClawtFedRegistry *self,
    const gchar      *id
){
    return g_hash_table_remove(self->invites, id);
}

GPtrArray *
clawt_fed_registry_list_invites(
    ClawtFedRegistry *self
){
    GPtrArray *out;
    GHashTableIter iter;
    gpointer value;

    out = g_ptr_array_new();
    g_hash_table_iter_init(&iter, self->invites);
    while (g_hash_table_iter_next(&iter, NULL, &value))
        g_ptr_array_add(out, value);
    return out;
}

guint
clawt_fed_registry_prune_invites(
    ClawtFedRegistry *self,
    gint64            now
){
    GHashTableIter iter;
    gpointer value;
    guint dropped = 0;

    g_hash_table_iter_init(&iter, self->invites);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        if (((ClawtFedPendingInvite *)value)->expires < now) {
            g_hash_table_iter_remove(&iter);
            dropped++;
        }
    }
    return dropped;
}
