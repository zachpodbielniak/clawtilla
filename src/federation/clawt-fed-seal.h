/*
 * clawt-fed-seal.h - Encrypting a body where nothing else does
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A signed envelope is authentic but readable.  On a tailnet WireGuard
 * hides it and through a tunnel TLS does (to the edge); on a plain LAN
 * nothing does.  Sealing encrypts the body with ChaCha20-Poly1305 under a
 * key the two fleets agree with X25519, so a LAN link is private too, and
 * a tunnel can be made opaque even to the edge that ends its TLS.
 *
 * Seal then sign: the sealed text is the body the stamp covers, so a
 * tampered ciphertext is refused by the signature before it is opened.
 * A sealed body is the JSON text `{"sealed":"<base64url nonce|ct|tag>"}`.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>
#include "federation/clawt-fleet-key.h"

G_BEGIN_DECLS

/**
 * clawt_fed_seal:
 * @key: our fleet key
 * @peer_seal_public: (array fixed-size=32): the peer's X25519 key
 * @body: the plaintext body, JSON text
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the sealed body
 */
gchar *clawt_fed_seal(ClawtFleetKey *key,
                      const guint8  *peer_seal_public,
                      const gchar   *body,
                      GError       **error);

/**
 * clawt_fed_body_is_sealed:
 * @body: (nullable): a body
 *
 * Returns: %TRUE if @body is a sealed body
 */
gboolean clawt_fed_body_is_sealed(const gchar *body);

/**
 * clawt_fed_unseal:
 * @key: our fleet key
 * @peer_seal_public: (array fixed-size=32): the peer's X25519 key
 * @body: a body, sealed or not
 * @error: (out) (optional): return location for a #GError
 *
 * Opens a sealed body; returns any other body unchanged, so a caller
 * reads both the same way and decides separately whether an unsealed one
 * was acceptable (clawt_fed_body_is_sealed()).
 *
 * Returns: (transfer full) (nullable): the plaintext, or %NULL when a
 *   sealed body does not open (tampered, or sealed for someone else)
 */
gchar *clawt_fed_unseal(ClawtFleetKey *key,
                        const guint8  *peer_seal_public,
                        const gchar   *body,
                        GError       **error);

G_END_DECLS
