/*
 * clawt-fed-seal.c - Encrypting a body where nothing else does
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "federation/clawt-fed-seal.h"

#include <string.h>
#include <json-glib/json-glib.h>
#include <nettle/chacha-poly1305.h>
#include <nettle/version.h>

#define SEAL_CONTEXT "clawtilla federation seal v1"

/* nettle 4.0 dropped the length argument from the digest functions. */
static void
seal_digest(
    struct chacha_poly1305_ctx *ctx,
    guint8                     *tag
){
#if NETTLE_VERSION_MAJOR >= 4
    chacha_poly1305_digest(ctx, tag);
#else
    chacha_poly1305_digest(ctx, CHACHA_POLY1305_DIGEST_SIZE, tag);
#endif
}

gchar *
clawt_fed_seal(
    ClawtFleetKey *key,
    const guint8  *peer_seal_public,
    const gchar   *body,
    GError       **error
){
    struct chacha_poly1305_ctx ctx;
    guint8 secret[CHACHA_POLY1305_KEY_SIZE];
    g_autofree guint8 *packed = NULL;
    g_autofree gchar *text = NULL;
    g_autoptr(JsonBuilder) builder = NULL;
    g_autoptr(JsonGenerator) generator = NULL;
    g_autoptr(JsonNode) root = NULL;
    gsize length;
    gsize total;

    if (!clawt_fleet_key_agree(key, peer_seal_public, SEAL_CONTEXT, secret)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_AUTH,
                    "the peer's seal key is unusable");
        return NULL;
    }

    /* nonce | ciphertext | tag; a random 96-bit nonce per body. */
    length = strlen(body);
    total = CHACHA_POLY1305_NONCE_SIZE + length + CHACHA_POLY1305_DIGEST_SIZE;
    packed = g_malloc(total);
    if (!clawt_fed_random_bytes(packed, CHACHA_POLY1305_NONCE_SIZE, error)) {
        memset(secret, 0, sizeof(secret));
        return NULL;
    }
    chacha_poly1305_set_key(&ctx, secret);
    chacha_poly1305_set_nonce(&ctx, packed);
    chacha_poly1305_encrypt(&ctx, length, packed + CHACHA_POLY1305_NONCE_SIZE,
                            (const guint8 *)body);
    seal_digest(&ctx, packed + CHACHA_POLY1305_NONCE_SIZE + length);
    memset(secret, 0, sizeof(secret));
    memset(&ctx, 0, sizeof(ctx));

    text = clawt_fed_b64url_encode(packed, total);
    builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "sealed");
    json_builder_add_string_value(builder, text);
    json_builder_end_object(builder);
    root = json_builder_get_root(builder);
    generator = json_generator_new();
    json_generator_set_root(generator, root);
    return json_generator_to_data(generator, NULL);
}

/* The sealed text of @body, or NULL when it is not a sealed body. */
static gchar *
sealed_text(
    const gchar *body
){
    g_autoptr(JsonParser) parser = NULL;
    JsonNode *root;
    JsonObject *object;
    JsonNode *member;

    if (body == NULL || strstr(body, "\"sealed\"") == NULL)
        return NULL;
    parser = json_parser_new();
    if (!json_parser_load_from_data(parser, body, -1, NULL))
        return NULL;
    root = json_parser_get_root(parser);
    if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root))
        return NULL;
    object = json_node_get_object(root);
    if (object == NULL || json_object_get_size(object) != 1)
        return NULL;
    member = json_object_get_member(object, "sealed");
    if (member == NULL || !JSON_NODE_HOLDS_VALUE(member)
        || json_node_get_value_type(member) != G_TYPE_STRING)
        return NULL;
    return g_strdup(json_node_get_string(member));
}

gboolean
clawt_fed_body_is_sealed(
    const gchar *body
){
    g_autofree gchar *text = sealed_text(body);

    return text != NULL;
}

gchar *
clawt_fed_unseal(
    ClawtFleetKey *key,
    const guint8  *peer_seal_public,
    const gchar   *body,
    GError       **error
){
    struct chacha_poly1305_ctx ctx;
    guint8 secret[CHACHA_POLY1305_KEY_SIZE];
    guint8 tag[CHACHA_POLY1305_DIGEST_SIZE];
    g_autofree gchar *text = NULL;
    g_autofree guint8 *packed = NULL;
    gchar *plain;
    gsize total;
    gsize length;

    text = sealed_text(body);
    if (text == NULL)
        return g_strdup(body);

    packed = clawt_fed_b64url_decode(text, &total);
    if (packed == NULL
        || total < CHACHA_POLY1305_NONCE_SIZE + CHACHA_POLY1305_DIGEST_SIZE) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_AUTH, "sealed body is malformed");
        return NULL;
    }
    if (!clawt_fleet_key_agree(key, peer_seal_public, SEAL_CONTEXT, secret)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_AUTH,
                    "the peer's seal key is unusable");
        return NULL;
    }
    length = total - CHACHA_POLY1305_NONCE_SIZE - CHACHA_POLY1305_DIGEST_SIZE;
    plain = g_malloc(length + 1);
    chacha_poly1305_set_key(&ctx, secret);
    chacha_poly1305_set_nonce(&ctx, packed);
    chacha_poly1305_decrypt(&ctx, length, (guint8 *)plain,
                            packed + CHACHA_POLY1305_NONCE_SIZE);
    seal_digest(&ctx, tag);
    memset(secret, 0, sizeof(secret));
    memset(&ctx, 0, sizeof(ctx));
    plain[length] = '\0';

    /* Constant-time tag comparison. */
    {
        guint8 diff = 0;
        gsize i;
        const guint8 *expect = packed + CHACHA_POLY1305_NONCE_SIZE + length;
        for (i = 0; i < CHACHA_POLY1305_DIGEST_SIZE; i++)
            diff |= tag[i] ^ expect[i];
        if (diff != 0 || !g_utf8_validate(plain, length, NULL)
            || memchr(plain, '\0', length) != NULL) {
            memset(plain, 0, length);
            g_free(plain);
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_AUTH,
                        "sealed body does not open");
            return NULL;
        }
    }
    return plain;
}
