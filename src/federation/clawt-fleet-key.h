/*
 * clawt-fleet-key.h - A fleet's own identity: an Ed25519 key
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Federation reaches fleets other people own, through a Cloudflare edge
 * that ends TLS, a LAN with no TLS at all, or a tailnet.  None of those
 * pipes says *which daemon* is on the other end, so the identity is a
 * key, not a connection: every request and every answer between fleets
 * is signed by the fleet's Ed25519 key, and a fleet's id is the
 * fingerprint of the first key it ever had.
 *
 * The 0.64.0 fleet id was a random string anybody could repeat; this one
 * cannot be claimed without the private half.
 *
 * The same seed also derives an X25519 key for sealing payloads on links
 * where nothing else encrypts them (a LAN with `federation.seal`).
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * CLAWT_FLEET_KEY_SIZE:
 *
 * Bytes in a public key, a private seed and an X25519 key.
 */
#define CLAWT_FLEET_KEY_SIZE (32)

/**
 * CLAWT_FLEET_SIGNATURE_SIZE:
 *
 * Bytes in an Ed25519 signature.
 */
#define CLAWT_FLEET_SIGNATURE_SIZE (64)

/**
 * CLAWT_FLEET_FINGERPRINT_LENGTH:
 *
 * Characters in a fingerprint: base32 of SHA-256 over the public key,
 * cut to 130 bits.  Long enough that nobody can grind a key to match,
 * short enough to read over a call.
 */
#define CLAWT_FLEET_FINGERPRINT_LENGTH (26)

#define CLAWT_TYPE_FLEET_KEY (clawt_fleet_key_get_type())

/**
 * ClawtFleetKey:
 *
 * A fleet's signing key, with the X25519 key derived from it.  Opaque
 * and reference-counted; the private half never leaves it.
 */
typedef struct _ClawtFleetKey ClawtFleetKey;

GType clawt_fleet_key_get_type(void) G_GNUC_CONST;

/**
 * clawt_fleet_key_new_from_seed:
 * @seed: (array fixed-size=32): the 32-byte private seed
 *
 * Returns: (transfer full): the key that seed describes
 */
ClawtFleetKey *clawt_fleet_key_new_from_seed(const guint8 *seed);

/**
 * clawt_fleet_key_generate:
 * @error: (out) (optional): return location for a #GError
 *
 * A fresh key from /dev/urandom.  Never from g_random_*: a predictable
 * seed is a fleet anybody can impersonate.
 *
 * Returns: (transfer full) (nullable): the key, or %NULL when the kernel
 *   pool could not be read
 */
ClawtFleetKey *clawt_fleet_key_generate(GError **error);

/**
 * clawt_fleet_key_load_or_create:
 * @path: where the seed lives (`<secrets.dir>/federation/fleet.ed25519`)
 * @error: (out) (optional): return location for a #GError
 *
 * Reads the seed at @path, or makes one and writes it 0600 (its directory
 * 0700) when there is none.  A file that exists but is not a seed is
 * refused, never replaced: replacing it would change the fleet's identity
 * under every peer that knows it.
 *
 * Returns: (transfer full) (nullable): the key
 */
ClawtFleetKey *clawt_fleet_key_load_or_create(const gchar  *path,
                                              GError      **error);

/**
 * clawt_fleet_key_save:
 * @key: a #ClawtFleetKey
 * @path: where to write its seed
 * @error: (out) (optional): return location for a #GError
 *
 * Writes the seed, 0600, replacing whatever is there -- for a rotation,
 * which archives the old file first.
 *
 * Returns: %TRUE on success
 */
gboolean clawt_fleet_key_save(ClawtFleetKey  *key,
                              const gchar    *path,
                              GError        **error);

/**
 * clawt_fleet_key_ref:
 * @key: a #ClawtFleetKey
 *
 * Returns: (transfer full): @key
 */
ClawtFleetKey *clawt_fleet_key_ref(ClawtFleetKey *key);

/**
 * clawt_fleet_key_unref:
 * @key: (transfer full): a #ClawtFleetKey
 *
 * Drops a reference; the seed is wiped when the last one goes.
 */
void clawt_fleet_key_unref(ClawtFleetKey *key);

/**
 * clawt_fleet_key_get_public:
 * @key: a #ClawtFleetKey
 *
 * Returns: (transfer none) (array fixed-size=32): the Ed25519 public key
 */
const guint8 *clawt_fleet_key_get_public(ClawtFleetKey *key);

/**
 * clawt_fleet_key_get_seal_public:
 * @key: a #ClawtFleetKey
 *
 * Returns: (transfer none) (array fixed-size=32): the X25519 public key
 */
const guint8 *clawt_fleet_key_get_seal_public(ClawtFleetKey *key);

/**
 * clawt_fleet_key_public_text:
 * @key: a #ClawtFleetKey
 *
 * Returns: (transfer full): the public key, base64url without padding
 */
gchar *clawt_fleet_key_public_text(ClawtFleetKey *key);

/**
 * clawt_fleet_key_seal_public_text:
 * @key: a #ClawtFleetKey
 *
 * Returns: (transfer full): the X25519 public key, base64url
 */
gchar *clawt_fleet_key_seal_public_text(ClawtFleetKey *key);

/**
 * clawt_fleet_key_fleet_id:
 * @key: a #ClawtFleetKey
 *
 * Returns: (transfer full): clawt_fleet_key_fingerprint() of this key
 */
gchar *clawt_fleet_key_fleet_id(ClawtFleetKey *key);

/**
 * clawt_fleet_key_sign:
 * @key: a #ClawtFleetKey
 * @data: (array length=length): what to sign
 * @length: bytes in @data
 * @signature: (out caller-allocates) (array fixed-size=64): the signature
 */
void clawt_fleet_key_sign(ClawtFleetKey *key,
                          const guint8  *data,
                          gsize          length,
                          guint8        *signature);

/**
 * clawt_fleet_key_sign_text:
 * @key: a #ClawtFleetKey
 * @text: a NUL-terminated string to sign
 *
 * Returns: (transfer full): the signature, base64url
 */
gchar *clawt_fleet_key_sign_text(ClawtFleetKey *key,
                                 const gchar   *text);

/**
 * clawt_fleet_key_verify:
 * @public_key: (array fixed-size=32): an Ed25519 public key
 * @data: (array length=length): what was signed
 * @length: bytes in @data
 * @signature: (array fixed-size=64): the signature
 *
 * Returns: %TRUE if @signature is @public_key's over @data
 */
gboolean clawt_fleet_key_verify(const guint8 *public_key,
                                const guint8 *data,
                                gsize         length,
                                const guint8 *signature);

/**
 * clawt_fleet_key_verify_text:
 * @public_key: (array fixed-size=32): an Ed25519 public key
 * @text: the signed string
 * @signature: (nullable): the signature, base64url
 *
 * Returns: %TRUE if @signature is @public_key's over @text; %FALSE for a
 *   missing or malformed signature as well as a wrong one
 */
gboolean clawt_fleet_key_verify_text(const guint8 *public_key,
                                     const gchar  *text,
                                     const gchar  *signature);

/**
 * clawt_fleet_key_agree:
 * @key: a #ClawtFleetKey
 * @peer_seal_public: (array fixed-size=32): the peer's X25519 public key
 * @context: (nullable): a label mixed into the result, so one pair of keys
 *   gives a different secret for each use
 * @out_secret: (out caller-allocates) (array fixed-size=32): the shared key
 *
 * X25519 between our seal key and the peer's, run through HMAC-SHA256
 * with @context.  Refuses an all-zero result, which is what a
 * low-order peer key produces.
 *
 * Returns: %TRUE if a usable secret was agreed
 */
gboolean clawt_fleet_key_agree(ClawtFleetKey *key,
                               const guint8  *peer_seal_public,
                               const gchar   *context,
                               guint8        *out_secret);

/**
 * clawt_fleet_key_fingerprint:
 * @public_key: (array fixed-size=32): an Ed25519 public key
 *
 * Returns: (transfer full): lowercase base32 of SHA-256(@public_key), the
 *   first %CLAWT_FLEET_FINGERPRINT_LENGTH characters
 */
gchar *clawt_fleet_key_fingerprint(const guint8 *public_key);

/**
 * clawt_fleet_key_safety_words:
 * @public_key: (array fixed-size=32): an Ed25519 public key
 *
 * clawt_fed_safety_words() of this key's fingerprint -- for a fleet's
 * *first* key that is its fleet id, which is what every client shows.
 *
 * Returns: (transfer full): e.g. "amber-otter-quill-harbor"
 */
gchar *clawt_fleet_key_safety_words(const guint8 *public_key);

/**
 * clawt_fed_safety_words:
 * @fleet_id: a fleet id (a fingerprint)
 *
 * The safety words for a fleet, from its id: the same before and after a
 * key rotation, and readable from an invite, which carries the id and
 * not the key.
 *
 * Returns: (transfer full): four words joined by `-`
 */
gchar *clawt_fed_safety_words(const gchar *fleet_id);

/**
 * clawt_fed_b64url_encode:
 * @data: (array length=length): bytes
 * @length: how many
 *
 * Returns: (transfer full): base64url, no padding
 */
gchar *clawt_fed_b64url_encode(const guint8 *data,
                               gsize         length);

/**
 * clawt_fed_b64url_decode:
 * @text: (nullable): base64url, padded or not
 * @out_length: (out): bytes decoded
 *
 * Returns: (transfer full) (nullable): the bytes, or %NULL when @text is
 *   not base64url (any character outside the alphabet)
 */
guint8 *clawt_fed_b64url_decode(const gchar *text,
                                gsize       *out_length);

/**
 * clawt_fed_decode_key:
 * @text: (nullable): a key, base64url
 * @out_key: (out caller-allocates) (array fixed-size=32): the key
 *
 * Returns: %TRUE if @text decodes to exactly 32 bytes
 */
gboolean clawt_fed_decode_key(const gchar *text,
                              guint8      *out_key);

/**
 * clawt_fed_random_bytes:
 * @buffer: (out caller-allocates) (array length=length): where to write
 * @length: how many bytes
 * @error: (out) (optional): return location for a #GError
 *
 * Bytes from /dev/urandom, refusing a short read.
 *
 * Returns: %TRUE on success
 */
gboolean clawt_fed_random_bytes(guint8  *buffer,
                                gsize    length,
                                GError **error);

/**
 * clawt_fed_random_text:
 * @bytes: how many random bytes to encode
 *
 * Returns: (transfer full) (nullable): base64url of @bytes fresh random
 *   bytes, or %NULL if the kernel pool could not be read
 */
gchar *clawt_fed_random_text(gsize bytes);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtFleetKey, clawt_fleet_key_unref)

G_END_DECLS
