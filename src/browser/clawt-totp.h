/*
 * clawt-totp.h - One-time codes (RFC 6238) from a stored seed
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A `vault:` entry of kind totp holds a seed, not a code.  The daemon
 * makes the code at the moment of a fill and types it like a password,
 * so an agent can get past a second factor without the seed -- or even
 * the code -- ever reaching it.  Counter-based (HOTP) keys are refused:
 * a code that advances on every use cannot be shared with the person's
 * own authenticator, which would drift out of step the first time the
 * daemon used one.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>

G_BEGIN_DECLS

/**
 * ClawtTotpKey:
 * @key: the decoded seed
 * @key_length: its length in bytes
 * @algorithm: %G_CHECKSUM_SHA1, %G_CHECKSUM_SHA256 or %G_CHECKSUM_SHA512
 * @digits: 6 to 8
 * @period: seconds per code, 10 to 300
 *
 * A parsed TOTP key.  Holds the seed: wipe it with clawt_totp_key_free(),
 * which zeroes @key before freeing it.
 */
typedef struct {
	guint8        *key;
	gsize          key_length;
	GChecksumType  algorithm;
	guint          digits;
	guint          period;
} ClawtTotpKey;

/**
 * clawt_totp_key_free:
 * @self: (nullable): a #ClawtTotpKey
 *
 * Zeroes the seed and frees @self.
 */
void clawt_totp_key_free(ClawtTotpKey *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtTotpKey, clawt_totp_key_free)

/**
 * clawt_totp_base32_decode:
 * @text: RFC 4648 base32; case, spaces, dashes and padding are ignored
 * @out_length: (out): the decoded length
 *
 * Returns: (transfer full) (nullable) (array length=out_length): the
 *   bytes, or %NULL when @text is empty or not base32
 */
guint8 *clawt_totp_base32_decode(const gchar *text, gsize *out_length);

/**
 * clawt_totp_key_parse: (skip)
 * @text: a base32 seed, or an `otpauth://totp/...` URI
 * @error: (out) (optional): return location for a #GError
 *
 * Parses a stored seed.  A URI's `algorithm` (SHA1, SHA256, SHA512),
 * `digits` (6-8) and `period` (10-300) are honoured; a bare seed is
 * SHA1, 6 digits, 30 seconds.  `otpauth://hotp/` is refused.  An error
 * never quotes @text.
 *
 * Returns: (transfer full) (nullable): the key, or %NULL
 */
ClawtTotpKey *clawt_totp_key_parse(const gchar *text, GError **error);

/**
 * clawt_totp_hotp:
 * @key: (array length=key_length): the seed
 * @key_length: its length
 * @algorithm: the HMAC hash
 * @counter: the moving factor
 * @digits: how many digits, 6 to 8
 *
 * RFC 4226: dynamic truncation of HMAC(@key, @counter), as decimal
 * digits with leading zeros.
 *
 * Returns: (transfer full): the code
 */
gchar *clawt_totp_hotp(const guint8  *key,
                       gsize          key_length,
                       GChecksumType  algorithm,
                       guint64        counter,
                       guint          digits);

/**
 * clawt_totp_code: (skip)
 * @key: a #ClawtTotpKey
 * @unix_seconds: the time to make the code for
 *
 * RFC 6238 with T0 = 0: clawt_totp_hotp() of @unix_seconds / period.
 *
 * Returns: (transfer full): the code
 */
gchar *clawt_totp_code(const ClawtTotpKey *key, gint64 unix_seconds);

G_END_DECLS
