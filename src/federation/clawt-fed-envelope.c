/*
 * clawt-fed-envelope.c - What one fleet says to another, signed
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "federation/clawt-fed-envelope.h"

#include <errno.h>
#include <string.h>

/*
 * Bounds on what a stamp member may hold.  A real one is far inside each;
 * a stamp past them is refused before any signature is checked, so a
 * peer cannot make us hash megabytes to find out it was lying.
 */
#define STAMP_MAX_ID      (64)
#define STAMP_MAX_KIND    (64)
#define STAMP_MAX_NONCE   (64)
#define STAMP_MAX_BINDING (256)
#define STAMP_MAX_SIG     (100)

G_DEFINE_BOXED_TYPE(ClawtFedStamp, clawt_fed_stamp,
                    clawt_fed_stamp_copy, clawt_fed_stamp_free)

ClawtFedStamp *
clawt_fed_stamp_copy(
    const ClawtFedStamp *stamp
){
    ClawtFedStamp *copy;

    copy = g_new0(ClawtFedStamp, 1);
    copy->kind = g_strdup(stamp->kind);
    copy->from = g_strdup(stamp->from);
    copy->to = g_strdup(stamp->to);
    copy->time = stamp->time;
    copy->nonce = g_strdup(stamp->nonce);
    copy->digest = g_strdup(stamp->digest);
    copy->binding = g_strdup(stamp->binding);
    copy->key_index = stamp->key_index;
    copy->signature = g_strdup(stamp->signature);
    return copy;
}

void
clawt_fed_stamp_free(
    ClawtFedStamp *stamp
){
    if (stamp == NULL)
        return;
    g_free(stamp->kind);
    g_free(stamp->from);
    g_free(stamp->to);
    g_free(stamp->nonce);
    g_free(stamp->digest);
    g_free(stamp->binding);
    g_free(stamp->signature);
    g_free(stamp);
}

gchar *
clawt_fed_digest(
    const gchar *body,
    gsize        length
){
    return g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                                       (const guchar *)(body != NULL ? body : ""),
                                       body != NULL ? length : 0);
}

gchar *
clawt_fed_signed_text(
    const ClawtFedStamp *stamp
){
    return g_strdup_printf("%s\n%s\n%s\n%s\n%" G_GINT64_FORMAT "\n%s\n%s\n%s",
                           CLAWT_FED_PROTOCOL,
                           stamp->kind != NULL ? stamp->kind : "",
                           stamp->from != NULL ? stamp->from : "",
                           stamp->to != NULL ? stamp->to : "",
                           stamp->time,
                           stamp->nonce != NULL ? stamp->nonce : "",
                           stamp->digest != NULL ? stamp->digest : "",
                           stamp->binding != NULL ? stamp->binding : "");
}

/* A member must be printable ASCII with no newline: the signed text is
 * newline-separated, and a newline inside one member could make two
 * different stamps sign the same text. */
static gboolean
member_is_clean(
    const gchar *value,
    gsize        max
){
    const gchar *p;

    if (value == NULL)
        return TRUE;
    if (strlen(value) > max)
        return FALSE;
    for (p = value; *p != '\0'; p++)
        if ((guchar)*p < 0x20 || (guchar)*p > 0x7e)
            return FALSE;
    return TRUE;
}

static gboolean
stamp_is_well_formed(
    const ClawtFedStamp *stamp
){
    return stamp->kind != NULL && *stamp->kind != '\0'
        && stamp->from != NULL && *stamp->from != '\0'
        && stamp->nonce != NULL && *stamp->nonce != '\0'
        && stamp->digest != NULL && strlen(stamp->digest) == 64
        && stamp->signature != NULL
        && member_is_clean(stamp->kind, STAMP_MAX_KIND)
        && member_is_clean(stamp->from, STAMP_MAX_ID)
        && member_is_clean(stamp->to, STAMP_MAX_ID)
        && member_is_clean(stamp->nonce, STAMP_MAX_NONCE)
        && member_is_clean(stamp->digest, 64)
        && member_is_clean(stamp->binding, STAMP_MAX_BINDING)
        && member_is_clean(stamp->signature, STAMP_MAX_SIG);
}

ClawtFedStamp *
clawt_fed_stamp_sign(
    ClawtFleetKey *key,
    guint          key_index,
    const gchar   *kind,
    const gchar   *from,
    const gchar   *to,
    const gchar   *body,
    const gchar   *binding,
    GError       **error
){
    g_autoptr(ClawtFedStamp) stamp = NULL;
    g_autofree gchar *text = NULL;
    gchar *nonce;

    nonce = clawt_fed_random_text(18);
    if (nonce == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "could not read /dev/urandom for a nonce");
        return NULL;
    }

    stamp = g_new0(ClawtFedStamp, 1);
    stamp->kind = g_strdup(kind);
    stamp->from = g_strdup(from);
    stamp->to = g_strdup(to != NULL ? to : "");
    stamp->time = g_get_real_time() / G_USEC_PER_SEC;
    stamp->nonce = nonce;
    stamp->digest = clawt_fed_digest(body, body != NULL ? strlen(body) : 0);
    stamp->binding = g_strdup(binding != NULL ? binding : "");
    stamp->key_index = key_index;

    text = clawt_fed_signed_text(stamp);
    stamp->signature = clawt_fleet_key_sign_text(key, text);
    return g_steal_pointer(&stamp);
}

gboolean
clawt_fed_stamp_check(
    const ClawtFedStamp *stamp,
    const guint8        *public_key,
    const gchar         *expect_to,
    const gchar         *body,
    const gchar         *binding,
    gint64               now,
    guint                skew,
    GError             **error
){
    g_autofree gchar *digest = NULL;
    g_autofree gchar *text = NULL;
    ClawtFedStamp expected;
    gint64 drift;

    if (!stamp_is_well_formed(stamp)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_AUTH, "malformed stamp");
        return FALSE;
    }

    /* Cheap checks first; the signature is the expensive one. */
    drift = now - stamp->time;
    if (drift > (gint64)skew || -drift > (gint64)skew) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_AUTH,
                    "stamp time is %" G_GINT64_FORMAT " s from ours "
                    "(window %u s); check both clocks", drift, skew);
        return FALSE;
    }
    if (expect_to != NULL && g_strcmp0(stamp->to, expect_to) != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_AUTH,
                    "stamp is addressed to another fleet");
        return FALSE;
    }
    digest = clawt_fed_digest(body, body != NULL ? strlen(body) : 0);
    if (!clawt_secure_equals(digest, stamp->digest)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_AUTH,
                    "body does not match its signed digest");
        return FALSE;
    }

    /*
     * The binding is the one *we* expect, never the one the stamp names:
     * HTTP does not send it (the method and path are already there), and
     * a stamp signed for another request then fails as a bad signature.
     */
    expected = *stamp;
    expected.binding = (gchar *)(binding != NULL ? binding : "");
    text = clawt_fed_signed_text(&expected);
    if (!clawt_fleet_key_verify_text(public_key, text, stamp->signature)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_AUTH, "bad signature");
        return FALSE;
    }
    return TRUE;
}

JsonObject *
clawt_fed_stamp_to_json(
    const ClawtFedStamp *stamp
){
    JsonObject *object;

    object = json_object_new();
    json_object_set_string_member(object, "kind", stamp->kind);
    json_object_set_string_member(object, "from", stamp->from);
    json_object_set_string_member(object, "to", stamp->to != NULL ? stamp->to : "");
    json_object_set_int_member(object, "time", stamp->time);
    json_object_set_string_member(object, "nonce", stamp->nonce);
    json_object_set_string_member(object, "digest", stamp->digest);
    json_object_set_string_member(object, "binding",
                                  stamp->binding != NULL ? stamp->binding : "");
    json_object_set_int_member(object, "key", stamp->key_index);
    json_object_set_string_member(object, "sig", stamp->signature);
    return object;
}

/* A string member, or NULL when absent or not a string. */
static const gchar *
string_member(
    JsonObject  *object,
    const gchar *name
){
    JsonNode *node;

    node = json_object_get_member(object, name);
    if (node == NULL || !JSON_NODE_HOLDS_VALUE(node)
        || json_node_get_value_type(node) != G_TYPE_STRING)
        return NULL;
    return json_node_get_string(node);
}

static gboolean
int_member(
    JsonObject  *object,
    const gchar *name,
    gint64      *out
){
    JsonNode *node;

    node = json_object_get_member(object, name);
    if (node == NULL || !JSON_NODE_HOLDS_VALUE(node)
        || json_node_get_value_type(node) != G_TYPE_INT64)
        return FALSE;
    *out = json_node_get_int(node);
    return TRUE;
}

ClawtFedStamp *
clawt_fed_stamp_from_json(
    JsonObject  *object,
    GError     **error
){
    g_autoptr(ClawtFedStamp) stamp = NULL;
    gint64 time = 0;
    gint64 key = 0;

    if (object == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_AUTH, "no stamp");
        return NULL;
    }
    stamp = g_new0(ClawtFedStamp, 1);
    stamp->kind = g_strdup(string_member(object, "kind"));
    stamp->from = g_strdup(string_member(object, "from"));
    stamp->to = g_strdup(string_member(object, "to"));
    stamp->nonce = g_strdup(string_member(object, "nonce"));
    stamp->digest = g_strdup(string_member(object, "digest"));
    stamp->binding = g_strdup(string_member(object, "binding"));
    stamp->signature = g_strdup(string_member(object, "sig"));
    if (!int_member(object, "time", &time) || !int_member(object, "key", &key)
        || key < 0 || key > 64 || !stamp_is_well_formed(stamp)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_AUTH, "malformed stamp");
        return NULL;
    }
    stamp->time = time;
    stamp->key_index = (guint)key;
    return g_steal_pointer(&stamp);
}

void
clawt_fed_stamp_to_headers(
    const ClawtFedStamp *stamp,
    ClawtFedHeaderFunc   func,
    gpointer             user_data
){
    g_autofree gchar *time = NULL;
    g_autofree gchar *from = NULL;

    time = g_strdup_printf("%" G_GINT64_FORMAT, stamp->time);
    /* The key index rides with the sender: `fleetid.0`. */
    from = g_strdup_printf("%s.%u", stamp->from, stamp->key_index);
    func("X-Clawt-Fed-Kind", stamp->kind, user_data);
    func("X-Clawt-Fed-From", from, user_data);
    func("X-Clawt-Fed-To", stamp->to != NULL ? stamp->to : "", user_data);
    func("X-Clawt-Fed-Time", time, user_data);
    func("X-Clawt-Fed-Nonce", stamp->nonce, user_data);
    func("X-Clawt-Fed-Digest", stamp->digest, user_data);
    func("X-Clawt-Fed-Sig", stamp->signature, user_data);
}

ClawtFedStamp *
clawt_fed_stamp_from_headers(
    ClawtFedHeaderLookup   lookup,
    gpointer               user_data,
    GError               **error
){
    g_autoptr(ClawtFedStamp) stamp = NULL;
    const gchar *from;
    const gchar *time;
    const gchar *dot;
    gchar *end = NULL;
    guint64 index;

    stamp = g_new0(ClawtFedStamp, 1);
    stamp->kind = g_strdup(lookup("X-Clawt-Fed-Kind", user_data));
    stamp->to = g_strdup(lookup("X-Clawt-Fed-To", user_data));
    stamp->nonce = g_strdup(lookup("X-Clawt-Fed-Nonce", user_data));
    stamp->digest = g_strdup(lookup("X-Clawt-Fed-Digest", user_data));
    stamp->signature = g_strdup(lookup("X-Clawt-Fed-Sig", user_data));
    stamp->binding = g_strdup("");
    from = lookup("X-Clawt-Fed-From", user_data);
    time = lookup("X-Clawt-Fed-Time", user_data);

    if (from == NULL || time == NULL || (dot = strrchr(from, '.')) == NULL)
        goto malformed;
    stamp->from = g_strndup(from, dot - from);
    errno = 0;
    index = g_ascii_strtoull(dot + 1, &end, 10);
    if (errno != 0 || end == dot + 1 || *end != '\0' || index > 64)
        goto malformed;
    stamp->key_index = (guint)index;
    errno = 0;
    stamp->time = g_ascii_strtoll(time, &end, 10);
    if (errno != 0 || end == time || *end != '\0')
        goto malformed;
    if (!stamp_is_well_formed(stamp))
        goto malformed;
    return g_steal_pointer(&stamp);

malformed:
    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_AUTH, "malformed stamp");
    return NULL;
}

JsonObject *
clawt_fed_envelope_build(
    const ClawtFedStamp *stamp,
    const gchar         *body
){
    JsonObject *envelope;

    envelope = json_object_new();
    json_object_set_object_member(envelope, "stamp", clawt_fed_stamp_to_json(stamp));
    json_object_set_string_member(envelope, "body", body != NULL ? body : "");
    return envelope;
}

ClawtFedStamp *
clawt_fed_envelope_split(
    JsonObject  *envelope,
    gchar      **out_body,
    GError     **error
){
    JsonNode *node;
    const gchar *body;
    ClawtFedStamp *stamp;

    *out_body = NULL;
    if (envelope == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_AUTH, "no envelope");
        return NULL;
    }
    node = json_object_get_member(envelope, "stamp");
    if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node)
        || json_node_get_object(node) == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_AUTH, "envelope has no stamp");
        return NULL;
    }
    body = string_member(envelope, "body");
    if (body == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_AUTH, "envelope has no body");
        return NULL;
    }
    stamp = clawt_fed_stamp_from_json(json_node_get_object(node), error);
    if (stamp == NULL)
        return NULL;
    *out_body = g_strdup(body);
    return stamp;
}

/* ---- the replay guard ------------------------------------------------ */

struct _ClawtFedReplayGuard {
    gint        ref_count;
    guint       skew;
    GHashTable *peers;     /* fleet id -> GHashTable(nonce -> expiry) */
};

G_DEFINE_BOXED_TYPE(ClawtFedReplayGuard, clawt_fed_replay_guard,
                    clawt_fed_replay_guard_ref, clawt_fed_replay_guard_unref)

ClawtFedReplayGuard *
clawt_fed_replay_guard_new(
    guint skew
){
    ClawtFedReplayGuard *guard;

    guard = g_new0(ClawtFedReplayGuard, 1);
    guard->ref_count = 1;
    guard->skew = skew;
    guard->peers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                         (GDestroyNotify)g_hash_table_unref);
    return guard;
}

ClawtFedReplayGuard *
clawt_fed_replay_guard_ref(
    ClawtFedReplayGuard *guard
){
    g_atomic_int_inc(&guard->ref_count);
    return guard;
}

void
clawt_fed_replay_guard_unref(
    ClawtFedReplayGuard *guard
){
    if (guard == NULL)
        return;
    if (g_atomic_int_dec_and_test(&guard->ref_count)) {
        g_hash_table_unref(guard->peers);
        g_free(guard);
    }
}

/* Drops expired nonces; if still at the bound, the one expiring first. */
static void
prune(
    GHashTable *nonces,
    gint64      now
){
    GHashTableIter iter;
    gpointer key;
    gpointer value;
    const gchar *oldest = NULL;
    gint64 oldest_expiry = G_MAXINT64;

    g_hash_table_iter_init(&iter, nonces);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        gint64 expiry = *(gint64 *)value;
        if (expiry < now)
            g_hash_table_iter_remove(&iter);
    }
    if (g_hash_table_size(nonces) < CLAWT_FED_NONCES_PER_PEER)
        return;
    g_hash_table_iter_init(&iter, nonces);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        gint64 expiry = *(gint64 *)value;
        if (expiry < oldest_expiry) {
            oldest_expiry = expiry;
            oldest = key;
        }
    }
    if (oldest != NULL)
        g_hash_table_remove(nonces, oldest);
}

static void
remember(
    ClawtFedReplayGuard *guard,
    const gchar         *peer,
    const gchar         *nonce,
    gint64               expiry
){
    GHashTable *nonces;
    gint64 *slot;

    nonces = g_hash_table_lookup(guard->peers, peer);
    if (nonces == NULL) {
        nonces = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
        g_hash_table_insert(guard->peers, g_strdup(peer), nonces);
    }
    slot = g_new(gint64, 1);
    *slot = expiry;
    g_hash_table_replace(nonces, g_strdup(nonce), slot);
}

gboolean
clawt_fed_replay_guard_admit(
    ClawtFedReplayGuard *guard,
    const gchar         *peer,
    const gchar         *nonce,
    gint64               time,
    gint64               now
){
    GHashTable *nonces;
    gint64 drift;

    drift = now - time;
    if (drift > (gint64)guard->skew || -drift > (gint64)guard->skew)
        return FALSE;
    nonces = g_hash_table_lookup(guard->peers, peer);
    if (nonces != NULL) {
        if (g_hash_table_contains(nonces, nonce))
            return FALSE;
        prune(nonces, now);
    }
    /* Remembered until it could no longer pass the time check anyway. */
    remember(guard, peer, nonce, time + guard->skew + 1);
    return TRUE;
}

gboolean
clawt_fed_replay_guard_save(
    ClawtFedReplayGuard *guard,
    const gchar         *path,
    gint64               now,
    GError             **error
){
    g_autoptr(GString) out = NULL;
    GHashTableIter peers;
    gpointer peer;
    gpointer table;

    out = g_string_new("# clawtilla federation replay guard: peer nonce expiry\n");
    g_hash_table_iter_init(&peers, guard->peers);
    while (g_hash_table_iter_next(&peers, &peer, &table)) {
        GHashTableIter nonces;
        gpointer nonce;
        gpointer expiry;

        g_hash_table_iter_init(&nonces, table);
        while (g_hash_table_iter_next(&nonces, &nonce, &expiry)) {
            if (*(gint64 *)expiry < now)
                continue;
            g_string_append_printf(out, "%s %s %" G_GINT64_FORMAT "\n",
                                   (const gchar *)peer, (const gchar *)nonce,
                                   *(gint64 *)expiry);
        }
    }
    return g_file_set_contents_full(path, out->str, out->len,
                                    G_FILE_SET_CONTENTS_CONSISTENT, 0600, error);
}

void
clawt_fed_replay_guard_load(
    ClawtFedReplayGuard *guard,
    const gchar         *path,
    gint64               now
){
    g_autofree gchar *contents = NULL;
    g_auto(GStrv) lines = NULL;
    guint i;

    if (!g_file_get_contents(path, &contents, NULL, NULL))
        return;
    lines = g_strsplit(contents, "\n", -1);
    for (i = 0; lines[i] != NULL; i++) {
        g_auto(GStrv) parts = NULL;
        gint64 expiry;
        gchar *end = NULL;

        if (lines[i][0] == '#' || lines[i][0] == '\0')
            continue;
        parts = g_strsplit(lines[i], " ", 3);
        if (g_strv_length(parts) != 3)
            continue;
        expiry = g_ascii_strtoll(parts[2], &end, 10);
        if (end == parts[2] || *end != '\0' || expiry < now)
            continue;
        if (!member_is_clean(parts[0], STAMP_MAX_ID)
            || !member_is_clean(parts[1], STAMP_MAX_NONCE))
            continue;
        remember(guard, parts[0], parts[1], expiry);
    }
}
