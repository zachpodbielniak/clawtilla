/*
 * test-secret-vault.c - Secret references, TOTP codes and vault entries
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The parts of "secrets the agent never sees" that need no browser: a
 * reference works once, for one agent in one conversation, and expires;
 * the TOTP codes match RFC 6238's own vectors; a vault entry without its
 * sites is never offered, and a host matches its sites and their
 * subdomains only.  The fill itself is in test-browser.c.
 */

#include "clawtilla.h"
#include "clawt-test-util.h"

#include <string.h>

#define NOW ((gint64)1700000000)

/* ── References ───────────────────────────────────────────────────── */

static void
test_ref_is_one_use(void)
{
	g_autoptr(ClawtSecretVault) vault = clawt_secret_vault_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *ref = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *again = NULL;

	ref = clawt_secret_vault_mint(vault, "surfer", "room-a", "github", NOW, &error);
	g_assert_no_error(error);
	g_assert_true(g_str_has_prefix(ref, CLAWT_SECRET_REF_PREFIX));
	g_assert_cmpuint(strlen(ref), ==, strlen(CLAWT_SECRET_REF_PREFIX) + 32);
	g_assert_cmpuint(clawt_secret_vault_count_live(vault, NOW), ==, 1);

	/* 1. Redeemed once: the entry's name, never a value. */
	name = clawt_secret_vault_redeem(vault, ref, "surfer", "room-a", NOW + 1, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(name, ==, "github");
	g_assert_cmpuint(clawt_secret_vault_count_live(vault, NOW + 1), ==, 0);

	/* 2. A second time: refused, saying it was used. */
	again = clawt_secret_vault_redeem(vault, ref, "surfer", "room-a", NOW + 2, &error);
	g_assert_null(again);
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED);
	g_assert_nonnull(strstr(error->message, "already used"));
}

static void
test_ref_is_bound_to_agent_and_conversation(void)
{
	g_autoptr(ClawtSecretVault) vault = clawt_secret_vault_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *for_room = NULL;
	g_autofree gchar *for_agent = NULL;
	g_autofree gchar *unnamed = NULL;
	gchar *name;

	for_room = clawt_secret_vault_mint(vault, "surfer", "room-a", "github", NOW, NULL);
	for_agent = clawt_secret_vault_mint(vault, "surfer", "room-a", "github", NOW, NULL);
	unnamed = clawt_secret_vault_mint(vault, "surfer", NULL, "github", NOW, NULL);
	g_assert_cmpstr(for_room, !=, for_agent);

	/* 1. Another conversation: refused, and spent -- the attempt is the
	 *    sign the reference travelled. */
	name = clawt_secret_vault_redeem(vault, for_room, "surfer", "room-b", NOW, &error);
	g_assert_null(name);
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED);
	g_assert_nonnull(strstr(error->message, "another conversation"));
	g_clear_error(&error);
	name = clawt_secret_vault_redeem(vault, for_room, "surfer", "room-a", NOW, &error);
	g_assert_null(name);
	g_assert_nonnull(strstr(error->message, "already used"));
	g_clear_error(&error);

	/* 2. Another agent, in the right room: refused and spent too. */
	name = clawt_secret_vault_redeem(vault, for_agent, "other", "room-a", NOW, &error);
	g_assert_null(name);
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	name = clawt_secret_vault_redeem(vault, for_agent, "surfer", "room-a", NOW, &error);
	g_assert_null(name);
	g_clear_error(&error);

	/* 3. No room is its own conversation, which a named room is not. */
	name = clawt_secret_vault_redeem(vault, unnamed, "surfer", "room-a", NOW, &error);
	g_assert_null(name);
	g_clear_error(&error);
	g_free(unnamed);
	unnamed = clawt_secret_vault_mint(vault, "surfer", NULL, "github", NOW, NULL);
	name = clawt_secret_vault_redeem(vault, unnamed, "surfer", NULL, NOW, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(name, ==, "github");
	g_free(name);
}

static void
test_ref_expires_and_is_forgotten(void)
{
	g_autoptr(ClawtSecretVault) vault = clawt_secret_vault_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *late = NULL;
	g_autofree gchar *dropped = NULL;
	gchar *name;

	/* 1. Expired: refused with a remedy. */
	late = clawt_secret_vault_mint(vault, "surfer", "r", "github", NOW, NULL);
	name = clawt_secret_vault_redeem(vault, late, "surfer", "r",
	                                 NOW + CLAWT_SECRET_REF_TTL_SECONDS, &error);
	g_assert_null(name);
	g_assert_nonnull(strstr(error->message, "expired"));
	g_clear_error(&error);

	/* 2. A stopped agent's references go with it. */
	dropped = clawt_secret_vault_mint(vault, "surfer", "r", "github", NOW, NULL);
	clawt_secret_vault_forget_agent(vault, "surfer");
	g_assert_cmpuint(clawt_secret_vault_count_live(vault, NOW), ==, 0);
	name = clawt_secret_vault_redeem(vault, dropped, "surfer", "r", NOW, &error);
	g_assert_null(name);
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND);
	g_clear_error(&error);

	/* 3. Not a reference at all: refused without a lookup. */
	name = clawt_secret_vault_redeem(vault, "hunter2", "surfer", "r", NOW, &error);
	g_assert_null(name);
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
	g_assert_null(strstr(error->message, "hunter2"));
}

/* ── TOTP ─────────────────────────────────────────────────────────── */

/*
 * RFC 6238 appendix B: the seeds are the ASCII digits repeated to the
 * hash's length, the codes eight digits long.
 */
static void
test_totp_rfc6238_vectors(void)
{
	static const gchar seed1[] = "12345678901234567890";
	static const gchar seed256[] = "12345678901234567890123456789012";
	static const gchar seed512[] =
		"1234567890123456789012345678901234567890123456789012345678901234";
	static const struct {
		gint64        time;
		const gchar  *sha1;
		const gchar  *sha256;
		const gchar  *sha512;
	} vectors[] = {
		{ 59,          "94287082", "46119246", "90693936" },
		{ 1111111109,  "07081804", "68084774", "25091201" },
		{ 1111111111,  "14050471", "67062674", "99943326" },
		{ 1234567890,  "89005924", "91819424", "93441116" },
		{ 2000000000,  "69279037", "90698825", "38618901" },
		{ G_GINT64_CONSTANT(20000000000), "65353130", "77737706", "47863826" }
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(vectors); i++) {
		guint64 counter = (guint64)vectors[i].time / 30;
		g_autofree gchar *sha1 = clawt_totp_hotp((const guint8 *)seed1, strlen(seed1),
		                                         G_CHECKSUM_SHA1, counter, 8);
		g_autofree gchar *sha256 = clawt_totp_hotp((const guint8 *)seed256, strlen(seed256),
		                                           G_CHECKSUM_SHA256, counter, 8);
		g_autofree gchar *sha512 = clawt_totp_hotp((const guint8 *)seed512, strlen(seed512),
		                                           G_CHECKSUM_SHA512, counter, 8);

		g_assert_cmpstr(sha1, ==, vectors[i].sha1);
		g_assert_cmpstr(sha256, ==, vectors[i].sha256);
		g_assert_cmpstr(sha512, ==, vectors[i].sha512);
	}
}

static void
test_totp_parse(void)
{
	/* "12345678901234567890" in base32 -- the RFC's SHA1 seed. */
	static const gchar base32[] = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ";
	g_autoptr(ClawtTotpKey) bare = NULL;
	g_autoptr(ClawtTotpKey) spaced = NULL;
	g_autoptr(ClawtTotpKey) uri = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *code = NULL;
	g_autofree gchar *uri_code = NULL;
	ClawtTotpKey *refused;

	/* 1. A bare seed: SHA1, six digits, thirty seconds. */
	bare = clawt_totp_key_parse(base32, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(bare->key_length, ==, 20);
	g_assert_cmpmem(bare->key, 20, "12345678901234567890", 20);
	code = clawt_totp_code(bare, 59);
	g_assert_cmpstr(code, ==, "287082");

	/* 2. As people copy it: lower case, grouped, padded. */
	spaced = clawt_totp_key_parse("gezd gnbv gy3t qojq gezd gnbv gy3t qojq====", &error);
	g_assert_no_error(error);
	g_assert_cmpmem(spaced->key, spaced->key_length, bare->key, bare->key_length);

	/* 3. An otpauth URI with its own algorithm, digits and period. */
	uri = clawt_totp_key_parse("otpauth://totp/Example:alice?secret=" "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ"
	                           "&algorithm=SHA1&digits=8&period=30&issuer=Example", &error);
	g_assert_no_error(error);
	g_assert_cmpuint(uri->digits, ==, 8);
	uri_code = clawt_totp_code(uri, 1111111109);
	g_assert_cmpstr(uri_code, ==, "07081804");

	/* 4. Refused, and never quoted back: HOTP, junk, too short, bad digits. */
	refused = clawt_totp_key_parse("otpauth://hotp/x?secret=GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ",
	                               &error);
	g_assert_null(refused);
	g_assert_nonnull(strstr(error->message, "hotp"));
	g_assert_null(strstr(error->message, "GEZDGNBV"));
	g_clear_error(&error);
	refused = clawt_totp_key_parse("not!base32", &error);
	g_assert_null(refused);
	g_assert_null(strstr(error->message, "not!base32"));
	g_clear_error(&error);
	refused = clawt_totp_key_parse("GEZDGNBV", &error);
	g_assert_null(refused);
	g_clear_error(&error);
	refused = clawt_totp_key_parse("otpauth://totp/x?secret=GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ"
	                               "&digits=9", &error);
	g_assert_null(refused);
	g_clear_error(&error);
}

/* ── Vault entries ────────────────────────────────────────────────── */

static void
test_vault_entries(void)
{
	g_autoptr(ClawtConfig) config = NULL;
	g_autoptr(GPtrArray) entries = NULL;
	g_autoptr(ClawtVaultEntry) github = NULL;
	g_autoptr(ClawtVaultEntry) missing = NULL;
	g_autoptr(GError) error = NULL;
	ClawtVaultEntry *code;

	/* The incomplete ones warn; nothing here is about warnings. */
	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*no sites*");
	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*neither password nor totp*");
	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*name and secret*");
	config = clawt_config_load_from_string(
		"vault:\n"
		"  - name: github\n"
		"    description: the bot's login\n"
		"    secret: {env: CLAWT_TEST_NOT_SET}\n"
		"    sites: [GitHub.com]\n"
		"  - name: github-2fa\n"
		"    kind: totp\n"
		"    secret: {file: github-totp}\n"
		"    sites: [github.com]\n"
		"  - name: nowhere\n"
		"    secret: {env: X}\n"
		"  - name: odd\n"
		"    kind: sms\n"
		"    secret: {env: X}\n"
		"    sites: [example.com]\n"
		"  - sites: [example.com]\n",
		&error);
	g_assert_no_error(error);
	entries = clawt_config_get_vault(config);
	g_test_assert_expected_messages();

	/* 1. Only the complete entries, in file order. */
	g_assert_cmpuint(entries->len, ==, 2);
	code = g_ptr_array_index(entries, 1);
	g_assert_cmpstr(code->name, ==, "github-2fa");
	g_assert_cmpint(code->kind, ==, CLAWT_VAULT_TOTP);

	/* 2. Looked up by name; the reference, never a value. */
	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*");
	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*");
	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*");
	github = clawt_config_get_vault_entry(config, "github");
	g_test_assert_expected_messages();
	g_assert_nonnull(github);
	g_assert_cmpstr(github->description, ==, "the bot's login");
	g_assert_cmpint(clawt_secret_ref_get_backend(github->secret), ==,
	                CLAWT_SECRET_BACKEND_ENV);
	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*");
	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*");
	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*");
	missing = clawt_config_get_vault_entry(config, "nowhere");
	g_test_assert_expected_messages();
	g_assert_null(missing);

	/* 3. Hosts: the site and its subdomains, without case; nothing else. */
	g_assert_true(clawt_vault_entry_allows_host(github, "github.com"));
	g_assert_true(clawt_vault_entry_allows_host(github, "GITHUB.COM"));
	g_assert_true(clawt_vault_entry_allows_host(github, "login.github.com"));
	g_assert_false(clawt_vault_entry_allows_host(github, "evilgithub.com"));
	g_assert_false(clawt_vault_entry_allows_host(github, "github.com.evil.example"));
	g_assert_false(clawt_vault_entry_allows_host(github, ""));
	g_assert_false(clawt_vault_entry_allows_host(github, NULL));
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);

	g_test_add_func("/secret-vault/ref/one-use", test_ref_is_one_use);
	g_test_add_func("/secret-vault/ref/bound-to-agent-and-conversation",
	                test_ref_is_bound_to_agent_and_conversation);
	g_test_add_func("/secret-vault/ref/expires-and-is-forgotten",
	                test_ref_expires_and_is_forgotten);
	g_test_add_func("/secret-vault/totp/rfc6238-vectors", test_totp_rfc6238_vectors);
	g_test_add_func("/secret-vault/totp/parse", test_totp_parse);
	g_test_add_func("/secret-vault/vault/entries", test_vault_entries);

	return g_test_run();
}
