/*
 * clawt-fleet-key.c - A fleet's own identity: an Ed25519 key
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * nettle is used for exactly three things -- Ed25519, X25519 and (in
 * clawt-fed-seal.c) ChaCha20-Poly1305 -- because GLib has none of them.
 * Hashing and HMAC stay in GLib: nettle 4.0 changed its digest API, and
 * keeping it to these calls keeps this file building against 3.x too.
 */

#include "clawtilla.h"
#include "federation/clawt-fleet-key.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <glib/gstdio.h>
#include <nettle/eddsa.h>
#include <nettle/curve25519.h>

struct _ClawtFleetKey {
    gint   ref_count;
    guint8 seed[CLAWT_FLEET_KEY_SIZE];
    guint8 public_key[CLAWT_FLEET_KEY_SIZE];
    guint8 seal_secret[CLAWT_FLEET_KEY_SIZE];
    guint8 seal_public[CLAWT_FLEET_KEY_SIZE];
};

G_DEFINE_BOXED_TYPE(ClawtFleetKey, clawt_fleet_key,
                    clawt_fleet_key_ref, clawt_fleet_key_unref)

/*
 * 256 short, distinct, easy-to-say words: one byte of fingerprint each.
 * No two share their first four letters, so a word half heard over a
 * call is still unambiguous.
 */
static const gchar * const safety_words[256] = {
    "acorn", "adder", "alpine", "amber", "anchor", "anvil", "apple", "arrow",
    "aspen", "atlas", "autumn", "badger", "bagel", "bamboo", "banjo", "barley",
    "basil", "beacon", "beetle", "bellow", "berry", "bison", "blaze", "blossom",
    "bramble", "brick", "bronze", "brook", "buckle", "bugle", "button", "cabin",
    "cactus", "camel", "candle", "canyon", "carrot", "castle", "cedar", "cello",
    "chalk", "cherry", "chess", "cider", "cinder", "circus", "clover", "cobalt",
    "comet", "copper", "coral", "cotton", "cougar", "coyote", "crane", "crater",
    "cricket", "crystal", "cumin", "cypress", "dahlia", "daisy", "delta", "denim",
    "desert", "dingo", "dolphin", "domino", "dragon", "drift", "drum", "dune",
    "eagle", "ember", "emerald", "engine", "falcon", "fennel", "ferret", "fiddle",
    "fig", "finch", "fjord", "flint", "flute", "forest", "fossil", "fox",
    "frost", "fudge", "galaxy", "garnet", "gazelle", "geyser", "ginger", "glacier",
    "goblet", "gopher", "granite", "grape", "gravel", "griffin", "grove", "gull",
    "hammer", "harbor", "harp", "hazel", "heron", "hickory", "honey", "hornet",
    "husky", "igloo", "indigo", "iris", "island", "ivory", "jackal", "jade",
    "jasmine", "jelly", "jigsaw", "juniper", "kayak", "kelp", "kettle", "kiwi",
    "koala", "lagoon", "lantern", "larch", "lava", "lemon", "lentil", "lilac",
    "linen", "lizard", "lobster", "locket", "lotus", "lunar", "lynx", "magnet",
    "mango", "maple", "marble", "meadow", "melon", "meteor", "mint", "mirror",
    "mole", "monsoon", "moose", "mosaic", "muffin", "nebula", "nectar", "needle",
    "nickel", "nutmeg", "oasis", "ocean", "olive", "onyx", "opal", "orbit",
    "orchid", "osprey", "otter", "oyster", "paddle", "panda", "papaya", "parrot",
    "pearl", "pebble", "pepper", "piano", "pickle", "pigeon", "pine", "planet",
    "plum", "pollen", "poppy", "prairie", "prism", "puffin", "pumpkin", "quail",
    "quartz", "quill", "quince", "rabbit", "radish", "raven", "reef", "ribbon",
    "ripple", "robin", "rocket", "rose", "ruby", "saddle", "saffron", "salmon",
    "sapphire", "scarab", "seal", "sequoia", "shadow", "shell", "sierra", "silver",
    "slate", "sloth", "spoon", "sparrow", "spruce", "squid", "stork", "summit",
    "sunset", "swan", "tango", "tapir", "thistle", "thunder", "tiger", "timber",
    "topaz", "torch", "toucan", "tulip", "tundra", "turnip", "umber", "valley",
    "velvet", "violet", "walnut", "walrus", "willow", "wombat", "yarrow", "zebra",
    "beaver", "dove", "goose", "nomad", "orange", "rhino", "tofu", "yak",
};

gboolean
clawt_fed_random_bytes(
    guint8      *buffer,
    gsize        length,
    GError     **error
){
    FILE *pool;
    gsize got;

    /* The kernel pool, never a userspace generator: these become keys. */
    pool = fopen("/dev/urandom", "rb");
    if (pool == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "could not open /dev/urandom: %s", g_strerror(errno));
        return FALSE;
    }
    got = fread(buffer, 1, length, pool);
    fclose(pool);
    if (got != length) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "short read from /dev/urandom: wanted %u bytes, got %u",
                    (guint)length, (guint)got);
        return FALSE;
    }
    return TRUE;
}

gchar *
clawt_fed_random_text(
    gsize        bytes
){
    g_autofree guint8 *buffer = NULL;
    gchar *text;

    buffer = g_malloc(bytes);
    if (!clawt_fed_random_bytes(buffer, bytes, NULL))
        return NULL;
    text = clawt_fed_b64url_encode(buffer, bytes);
    memset(buffer, 0, bytes);
    return text;
}

gchar *
clawt_fed_b64url_encode(
    const guint8 *data,
    gsize         length
){
    gchar *text;
    gchar *p;

    text = g_base64_encode(data, length);
    /* RFC 4648 section 5: a URL- and header-safe alphabet, no padding. */
    for (p = text; *p != '\0'; p++) {
        if (*p == '+')
            *p = '-';
        else if (*p == '/')
            *p = '_';
        else if (*p == '=') {
            *p = '\0';
            break;
        }
    }
    return text;
}

guint8 *
clawt_fed_b64url_decode(
    const gchar *text,
    gsize       *out_length
){
    GString *standard;
    const gchar *p;
    gsize len;
    guint8 *bytes;

    *out_length = 0;
    if (text == NULL)
        return NULL;

    /*
     * g_base64_decode() silently skips characters outside its alphabet,
     * so a mangled signature would decode to *something*.  Check every
     * character first and translate to the standard alphabet.
     */
    standard = g_string_new(NULL);
    for (p = text; *p != '\0'; p++) {
        gchar c = *p;
        if (g_ascii_isalnum(c))
            g_string_append_c(standard, c);
        else if (c == '-' || c == '+')
            g_string_append_c(standard, '+');
        else if (c == '_' || c == '/')
            g_string_append_c(standard, '/');
        else if (c == '=')
            break;
        else {
            g_string_free(standard, TRUE);
            return NULL;
        }
    }
    len = standard->len;
    if (len % 4 == 1) {
        g_string_free(standard, TRUE);
        return NULL;
    }
    while (standard->len % 4 != 0)
        g_string_append_c(standard, '=');

    bytes = g_base64_decode(standard->str, out_length);
    g_string_free(standard, TRUE);
    return bytes;
}

gboolean
clawt_fed_decode_key(
    const gchar *text,
    guint8      *out_key
){
    g_autofree guint8 *bytes = NULL;
    gsize length;

    bytes = clawt_fed_b64url_decode(text, &length);
    if (bytes == NULL || length != CLAWT_FLEET_KEY_SIZE)
        return FALSE;
    memcpy(out_key, bytes, CLAWT_FLEET_KEY_SIZE);
    return TRUE;
}

/* HMAC-SHA256 of @data under @label, into a 32-byte buffer. */
static void
hmac_sha256(
    const gchar  *label,
    const guint8 *data,
    gsize         length,
    guint8       *out
){
    GHmac *hmac;
    gsize digest_len = 32;

    hmac = g_hmac_new(G_CHECKSUM_SHA256, (const guchar *)label, strlen(label));
    g_hmac_update(hmac, data, length);
    g_hmac_get_digest(hmac, out, &digest_len);
    g_hmac_unref(hmac);
}

ClawtFleetKey *
clawt_fleet_key_new_from_seed(
    const guint8 *seed
){
    ClawtFleetKey *key;

    key = g_new0(ClawtFleetKey, 1);
    key->ref_count = 1;
    memcpy(key->seed, seed, CLAWT_FLEET_KEY_SIZE);
    ed25519_sha512_public_key(key->public_key, key->seed);

    /*
     * The seal key is derived rather than stored, so a fleet has one
     * secret to keep.  A distinct label keeps the two uses of the seed
     * apart: nothing signed can be mistaken for key agreement.
     */
    hmac_sha256("clawtilla federation x25519 v1", key->seed,
                CLAWT_FLEET_KEY_SIZE, key->seal_secret);
    curve25519_mul_g(key->seal_public, key->seal_secret);
    return key;
}

ClawtFleetKey *
clawt_fleet_key_generate(
    GError **error
){
    guint8 seed[CLAWT_FLEET_KEY_SIZE];
    ClawtFleetKey *key;

    if (!clawt_fed_random_bytes(seed, sizeof(seed), error))
        return NULL;
    key = clawt_fleet_key_new_from_seed(seed);
    memset(seed, 0, sizeof(seed));
    return key;
}

ClawtFleetKey *
clawt_fleet_key_load_or_create(
    const gchar  *path,
    GError      **error
){
    g_autofree gchar *contents = NULL;
    g_autofree gchar *dir = NULL;
    g_autofree gchar *text = NULL;
    g_autoptr(GError) local = NULL;
    guint8 seed[CLAWT_FLEET_KEY_SIZE];
    ClawtFleetKey *key;
    gsize length;

    if (g_file_get_contents(path, &contents, &length, &local)) {
        g_strstrip(contents);
        if (!clawt_fed_decode_key(contents, seed)) {
            /* Refuse; replacing it would change who this fleet is. */
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_SECRET,
                        "%s is not a fleet key (expected 32 bytes of "
                        "base64url); move it aside to make a new identity",
                        path);
            memset(contents, 0, length);
            return NULL;
        }
        memset(contents, 0, length);
        key = clawt_fleet_key_new_from_seed(seed);
        memset(seed, 0, sizeof(seed));
        return key;
    }
    if (!g_error_matches(local, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
        g_propagate_error(error, g_steal_pointer(&local));
        return NULL;
    }

    /* None yet: make one, readable by the daemon's user alone. */
    dir = g_path_get_dirname(path);
    if (g_mkdir_with_parents(dir, 0700) != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_SECRET,
                    "cannot create %s: %s", dir, g_strerror(errno));
        return NULL;
    }
    key = clawt_fleet_key_generate(error);
    if (key == NULL)
        return NULL;
    text = clawt_fed_b64url_encode(key->seed, CLAWT_FLEET_KEY_SIZE);
    if (!g_file_set_contents_full(path, text, -1,
                                  G_FILE_SET_CONTENTS_CONSISTENT, 0600, error)) {
        memset(text, 0, strlen(text));
        clawt_fleet_key_unref(key);
        return NULL;
    }
    memset(text, 0, strlen(text));
    return key;
}

gboolean
clawt_fleet_key_save(
    ClawtFleetKey  *key,
    const gchar    *path,
    GError        **error
){
    g_autofree gchar *text = NULL;
    g_autofree gchar *dir = NULL;
    gboolean ok;

    dir = g_path_get_dirname(path);
    if (g_mkdir_with_parents(dir, 0700) != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_SECRET,
                    "cannot create %s: %s", dir, g_strerror(errno));
        return FALSE;
    }
    text = clawt_fed_b64url_encode(key->seed, CLAWT_FLEET_KEY_SIZE);
    ok = g_file_set_contents_full(path, text, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, error);
    memset(text, 0, strlen(text));
    return ok;
}

ClawtFleetKey *
clawt_fleet_key_ref(
    ClawtFleetKey *key
){
    g_return_val_if_fail(key != NULL, NULL);
    g_atomic_int_inc(&key->ref_count);
    return key;
}

void
clawt_fleet_key_unref(
    ClawtFleetKey *key
){
    if (key == NULL)
        return;
    if (g_atomic_int_dec_and_test(&key->ref_count)) {
        memset(key, 0, sizeof(*key));
        g_free(key);
    }
}

const guint8 *
clawt_fleet_key_get_public(
    ClawtFleetKey *key
){
    return key->public_key;
}

const guint8 *
clawt_fleet_key_get_seal_public(
    ClawtFleetKey *key
){
    return key->seal_public;
}

gchar *
clawt_fleet_key_public_text(
    ClawtFleetKey *key
){
    return clawt_fed_b64url_encode(key->public_key, CLAWT_FLEET_KEY_SIZE);
}

gchar *
clawt_fleet_key_seal_public_text(
    ClawtFleetKey *key
){
    return clawt_fed_b64url_encode(key->seal_public, CLAWT_FLEET_KEY_SIZE);
}

gchar *
clawt_fleet_key_fleet_id(
    ClawtFleetKey *key
){
    return clawt_fleet_key_fingerprint(key->public_key);
}

void
clawt_fleet_key_sign(
    ClawtFleetKey *key,
    const guint8  *data,
    gsize          length,
    guint8        *signature
){
    ed25519_sha512_sign(key->public_key, key->seed, length, data, signature);
}

gchar *
clawt_fleet_key_sign_text(
    ClawtFleetKey *key,
    const gchar   *text
){
    guint8 signature[CLAWT_FLEET_SIGNATURE_SIZE];

    clawt_fleet_key_sign(key, (const guint8 *)text, strlen(text), signature);
    return clawt_fed_b64url_encode(signature, sizeof(signature));
}

gboolean
clawt_fleet_key_verify(
    const guint8 *public_key,
    const guint8 *data,
    gsize         length,
    const guint8 *signature
){
    return ed25519_sha512_verify(public_key, length, data, signature) == 1;
}

gboolean
clawt_fleet_key_verify_text(
    const guint8 *public_key,
    const gchar  *text,
    const gchar  *signature
){
    g_autofree guint8 *sig = NULL;
    gsize length;

    if (text == NULL || signature == NULL)
        return FALSE;
    sig = clawt_fed_b64url_decode(signature, &length);
    if (sig == NULL || length != CLAWT_FLEET_SIGNATURE_SIZE)
        return FALSE;
    return clawt_fleet_key_verify(public_key, (const guint8 *)text,
                                  strlen(text), sig);
}

gboolean
clawt_fleet_key_agree(
    ClawtFleetKey *key,
    const guint8  *peer_seal_public,
    const gchar   *context,
    guint8        *out_secret
){
    guint8 shared[CLAWT_FLEET_KEY_SIZE];
    guint8 zero[CLAWT_FLEET_KEY_SIZE];
    GHmac *hmac;
    gsize digest_len = 32;
    const gchar *label;

    curve25519_mul(shared, key->seal_secret, peer_seal_public);
    memset(zero, 0, sizeof(zero));
    /* A low-order point gives all zeros: a "secret" an attacker knows. */
    if (memcmp(shared, zero, sizeof(zero)) == 0)
        return FALSE;

    label = context != NULL ? context : "clawtilla federation seal v1";
    hmac = g_hmac_new(G_CHECKSUM_SHA256, shared, sizeof(shared));
    g_hmac_update(hmac, (const guchar *)label, strlen(label));
    g_hmac_get_digest(hmac, out_secret, &digest_len);
    g_hmac_unref(hmac);
    memset(shared, 0, sizeof(shared));
    return TRUE;
}

gchar *
clawt_fleet_key_fingerprint(
    const guint8 *public_key
){
    static const gchar alphabet[] = "abcdefghijklmnopqrstuvwxyz234567";
    GChecksum *sum;
    guint8 digest[32];
    gsize digest_len = sizeof(digest);
    GString *out;
    guint bits = 0;
    guint value = 0;
    gsize i;

    sum = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(sum, public_key, CLAWT_FLEET_KEY_SIZE);
    g_checksum_get_digest(sum, digest, &digest_len);
    g_checksum_free(sum);

    /* RFC 4648 base32, lowercase, five bits at a time. */
    out = g_string_new(NULL);
    for (i = 0; i < digest_len && out->len < CLAWT_FLEET_FINGERPRINT_LENGTH; i++) {
        value = (value << 8) | digest[i];
        bits += 8;
        while (bits >= 5 && out->len < CLAWT_FLEET_FINGERPRINT_LENGTH) {
            g_string_append_c(out, alphabet[(value >> (bits - 5)) & 31]);
            bits -= 5;
        }
    }
    return g_string_free(out, FALSE);
}

gchar *
clawt_fed_safety_words(
    const gchar *fleet_id
){
    GChecksum *sum;
    guint8 digest[32];
    gsize digest_len = sizeof(digest);

    sum = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(sum, (const guchar *)"clawtilla safety words|", -1);
    g_checksum_update(sum, (const guchar *)(fleet_id != NULL ? fleet_id : ""), -1);
    g_checksum_get_digest(sum, digest, &digest_len);
    g_checksum_free(sum);

    return g_strdup_printf("%s-%s-%s-%s",
                           safety_words[digest[0]], safety_words[digest[1]],
                           safety_words[digest[2]], safety_words[digest[3]]);
}

gchar *
clawt_fleet_key_safety_words(
    const guint8 *public_key
){
    g_autofree gchar *fleet_id = clawt_fleet_key_fingerprint(public_key);

    return clawt_fed_safety_words(fleet_id);
}
