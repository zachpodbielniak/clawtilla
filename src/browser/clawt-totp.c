/*
 * clawt-totp.c - One-time codes (RFC 6238) from a stored seed
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"

#include <string.h>

void
clawt_totp_key_free(
	ClawtTotpKey *self
){
	if (self == NULL)
		return;
	if (self->key != NULL)
		memset(self->key, 0, self->key_length);
	g_free(self->key);
	g_free(self);
}

guint8 *
clawt_totp_base32_decode(
	const gchar *text,
	gsize       *out_length
){
	static const gchar alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
	GByteArray *out;
	guint32 buffer;
	guint bits;
	const gchar *p;

	*out_length = 0;
	if (text == NULL)
		return NULL;

	out = g_byte_array_new();
	buffer = 0;
	bits = 0;
	for (p = text; *p != '\0'; p++) {
		const gchar *found;
		gchar c = g_ascii_toupper(*p);

		/* Spaces and dashes are how seeds are printed for people to copy. */
		if (c == ' ' || c == '-' || c == '\t')
			continue;
		if (c == '=')
			break;
		found = strchr(alphabet, c);
		if (found == NULL) {
			memset(out->data, 0, out->len);
			g_byte_array_free(out, TRUE);
			return NULL;
		}
		buffer = (buffer << 5) | (guint32)(found - alphabet);
		bits += 5;
		if (bits >= 8) {
			guint8 byte;

			bits -= 8;
			byte = (guint8)((buffer >> bits) & 0xff);
			g_byte_array_append(out, &byte, 1);
		}
	}
	buffer = 0;

	if (out->len == 0) {
		g_byte_array_free(out, TRUE);
		return NULL;
	}
	*out_length = out->len;
	return g_byte_array_free(out, FALSE);
}

/* An integer query value, or @fallback when absent; FALSE when malformed. */
static gboolean
query_uint(
	GHashTable  *query,
	const gchar *name,
	guint        fallback,
	guint       *out
){
	const gchar *text = g_hash_table_lookup(query, name);
	guint64 value = 0;

	if (text == NULL) {
		*out = fallback;
		return TRUE;
	}
	if (!g_ascii_string_to_unsigned(text, 10, 0, G_MAXUINT, &value, NULL))
		return FALSE;
	*out = (guint)value;
	return TRUE;
}

ClawtTotpKey *
clawt_totp_key_parse(
	const gchar  *text,
	GError      **error
){
	g_autoptr(ClawtTotpKey) key = NULL;
	g_autoptr(GHashTable) query = NULL;
	g_autofree gchar *trimmed = NULL;
	const gchar *seed;

	if (text == NULL) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		                    "the TOTP seed is empty");
		return NULL;
	}
	trimmed = g_strstrip(g_strdup(text));

	key = g_new0(ClawtTotpKey, 1);
	key->algorithm = G_CHECKSUM_SHA1;
	key->digits = 6;
	key->period = 30;
	seed = trimmed;

	/*
	 * 1. An otpauth URI: what an enrolment QR code holds.  Every error
	 *    below names what is wrong with it, never a part of it.
	 */
	if (g_ascii_strncasecmp(trimmed, "otpauth:", 8) == 0) {
		g_autoptr(GUri) uri = g_uri_parse(trimmed, G_URI_FLAGS_NONE, NULL);
		const gchar *algorithm;

		if (uri == NULL) {
			g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
			                    "the TOTP seed is an otpauth URI that does not parse");
			memset(trimmed, 0, strlen(trimmed));
			return NULL;
		}
		if (g_ascii_strcasecmp(g_uri_get_host(uri) != NULL ? g_uri_get_host(uri) : "",
		                       "totp") != 0) {
			g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
			                    "only otpauth://totp/ keys are taken: a counter-based "
			                    "(hotp) code would drift from the person's own "
			                    "authenticator");
			memset(trimmed, 0, strlen(trimmed));
			return NULL;
		}
		query = g_uri_get_query(uri) != NULL
			? g_uri_parse_params(g_uri_get_query(uri), -1, "&", G_URI_PARAMS_CASE_INSENSITIVE,
			                     NULL)
			: NULL;
		if (query == NULL || g_hash_table_lookup(query, "secret") == NULL) {
			g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
			                    "the otpauth URI has no secret");
			memset(trimmed, 0, strlen(trimmed));
			return NULL;
		}
		algorithm = g_hash_table_lookup(query, "algorithm");
		if (algorithm == NULL || g_ascii_strcasecmp(algorithm, "SHA1") == 0)
			key->algorithm = G_CHECKSUM_SHA1;
		else if (g_ascii_strcasecmp(algorithm, "SHA256") == 0)
			key->algorithm = G_CHECKSUM_SHA256;
		else if (g_ascii_strcasecmp(algorithm, "SHA512") == 0)
			key->algorithm = G_CHECKSUM_SHA512;
		else {
			g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
			                    "the otpauth algorithm is not SHA1, SHA256 or SHA512");
			memset(trimmed, 0, strlen(trimmed));
			return NULL;
		}
		if (!query_uint(query, "digits", 6, &key->digits) ||
		    !query_uint(query, "period", 30, &key->period)) {
			g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
			                    "the otpauth digits or period is not a number");
			memset(trimmed, 0, strlen(trimmed));
			return NULL;
		}
		seed = g_hash_table_lookup(query, "secret");
	}

	/* 2. The seed itself, and the ranges authenticators agree on. */
	if (key->digits < 6 || key->digits > 8 || key->period < 10 || key->period > 300) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		                    "TOTP digits must be 6 to 8 and the period 10 to 300 seconds");
		memset(trimmed, 0, strlen(trimmed));
		return NULL;
	}
	key->key = clawt_totp_base32_decode(seed, &key->key_length);
	memset(trimmed, 0, strlen(trimmed));
	if (key->key == NULL || key->key_length < 10) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		                    "the TOTP seed is not base32, or is shorter than 80 bits");
		return NULL;
	}
	return (ClawtTotpKey *)g_steal_pointer(&key);
}

gchar *
clawt_totp_hotp(
	const guint8  *key,
	gsize          key_length,
	GChecksumType  algorithm,
	guint64        counter,
	guint          digits
){
	g_autoptr(GHmac) hmac = NULL;
	guint8 message[8];
	guint8 digest[64];
	gsize digest_length = sizeof(digest);
	guint offset;
	guint32 binary;
	guint32 modulus;
	guint i;

	g_return_val_if_fail(key != NULL, NULL);
	g_return_val_if_fail(digits >= 6 && digits <= 8, NULL);

	/* The counter, big-endian, as RFC 4226 section 5.2 says. */
	for (i = 0; i < 8; i++)
		message[7 - i] = (guint8)((counter >> (8 * i)) & 0xff);

	hmac = g_hmac_new(algorithm, key, key_length);
	g_hmac_update(hmac, message, sizeof(message));
	g_hmac_get_digest(hmac, digest, &digest_length);

	/* Dynamic truncation: the low nibble of the last byte picks four. */
	offset = (guint)(digest[digest_length - 1] & 0x0f);
	binary = ((guint32)(digest[offset] & 0x7f) << 24) |
	         ((guint32)digest[offset + 1] << 16) |
	         ((guint32)digest[offset + 2] << 8) |
	         (guint32)digest[offset + 3];
	memset(digest, 0, sizeof(digest));

	modulus = 1;
	for (i = 0; i < digits; i++)
		modulus *= 10;
	return g_strdup_printf("%0*u", (gint)digits, (guint)(binary % modulus));
}

gchar *
clawt_totp_code(
	const ClawtTotpKey *key,
	gint64              unix_seconds
){
	g_return_val_if_fail(key != NULL, NULL);
	g_return_val_if_fail(key->period > 0, NULL);

	return clawt_totp_hotp(key->key, key->key_length, key->algorithm,
	                       (guint64)(unix_seconds < 0 ? 0 : unix_seconds) / key->period,
	                       key->digits);
}
