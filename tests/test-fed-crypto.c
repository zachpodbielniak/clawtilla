/*
 * test-fed-crypto.c - Fleet keys, signed envelopes, replay and sealing
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Federation's security is in the envelope, not the pipe, so the
 * envelope is tested on its own: a known Ed25519 vector (so the wrapper
 * is the real algorithm, not merely self-consistent), every signed
 * member tampered with one at a time, the time window, a nonce replayed
 * (including across a save and load, which is a restart), the guard's
 * bound, and sealed bodies that open only for the right fleet.
 */

#include "clawtilla.h"
#include "clawt-test-util.h"
#include <string.h>

static guint8 *
unhex(
    const gchar *hex,
    gsize       *out_len
){
    gsize n = strlen(hex) / 2;
    guint8 *out = g_malloc(n);
    gsize i;

    for (i = 0; i < n; i++)
        out[i] = (guint8)((g_ascii_xdigit_value(hex[2 * i]) << 4)
                          | g_ascii_xdigit_value(hex[2 * i + 1]));
    *out_len = n;
    return out;
}

/* RFC 8032 section 7.1, TEST 1: the empty message. */
static void
test_rfc8032_vector(void)
{
    g_autofree guint8 *seed = NULL;
    g_autofree guint8 *pub = NULL;
    g_autofree guint8 *sig = NULL;
    g_autoptr(ClawtFleetKey) key = NULL;
    guint8 made[CLAWT_FLEET_SIGNATURE_SIZE];
    gsize n;

    seed = unhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", &n);
    pub = unhex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", &n);
    sig = unhex("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065"
                "224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24"
                "655141438e7a100b", &n);

    key = clawt_fleet_key_new_from_seed(seed);
    g_assert_cmpmem(clawt_fleet_key_get_public(key), 32, pub, 32);
    clawt_fleet_key_sign(key, (const guint8 *)"", 0, made);
    g_assert_cmpmem(made, 64, sig, 64);
    g_assert_true(clawt_fleet_key_verify(pub, (const guint8 *)"", 0, sig));
    sig[0] ^= 1;
    g_assert_false(clawt_fleet_key_verify(pub, (const guint8 *)"", 0, sig));
}

/* The seed is written once, 0600, and the same identity comes back. */
static void
test_load_or_create(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-fed-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(dir, "federation", "fleet.ed25519", NULL);
    g_autoptr(ClawtFleetKey) first = NULL;
    g_autoptr(ClawtFleetKey) second = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *id1 = NULL;
    g_autofree gchar *id2 = NULL;
    g_autofree gchar *words = NULL;
    GStatBuf st;

    first = clawt_fleet_key_load_or_create(path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(g_stat(path, &st), ==, 0);
    g_assert_cmpint(st.st_mode & 0777, ==, 0600);
    second = clawt_fleet_key_load_or_create(path, &error);
    g_assert_no_error(error);
    id1 = clawt_fleet_key_fleet_id(first);
    id2 = clawt_fleet_key_fleet_id(second);
    g_assert_cmpstr(id1, ==, id2);
    g_assert_cmpuint(strlen(id1), ==, CLAWT_FLEET_FINGERPRINT_LENGTH);

    words = clawt_fleet_key_safety_words(clawt_fleet_key_get_public(first));
    g_assert_nonnull(strchr(words, '-'));

    /* A damaged seed is refused, never silently replaced. */
    g_assert_true(g_file_set_contents(path, "not a key", -1, NULL));
    g_assert_null(clawt_fleet_key_load_or_create(path, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_SECRET);

    clawt_test_remove_tree(dir);
}

typedef struct {
    ClawtFleetKey *a;
    ClawtFleetKey *b;
    gchar         *a_id;
    gchar         *b_id;
} Pair;

static void
pair_init(Pair *p)
{
    p->a = clawt_fleet_key_generate(NULL);
    p->b = clawt_fleet_key_generate(NULL);
    p->a_id = clawt_fleet_key_fleet_id(p->a);
    p->b_id = clawt_fleet_key_fleet_id(p->b);
}

static void
pair_clear(Pair *p)
{
    clawt_fleet_key_unref(p->a);
    clawt_fleet_key_unref(p->b);
    g_free(p->a_id);
    g_free(p->b_id);
}

/* A good stamp checks; each member changed alone does not. */
static void
test_tamper_each_member(void)
{
    Pair p;
    const gchar *body = "{\"text\":\"hello\"}";
    const gchar *binding = "POST /fed/v1/envelope";
    g_autoptr(ClawtFedStamp) stamp = NULL;
    g_autoptr(GError) error = NULL;
    gint64 now;
    guint i;

    pair_init(&p);
    stamp = clawt_fed_stamp_sign(p.a, 0, "message", p.a_id, p.b_id, body, binding, &error);
    g_assert_no_error(error);
    now = stamp->time;
    g_assert_true(clawt_fed_stamp_check(stamp, clawt_fleet_key_get_public(p.a),
                                        p.b_id, body, binding, now, 300, &error));
    g_assert_no_error(error);

    for (i = 0; i < 8; i++) {
        g_autoptr(ClawtFedStamp) bad = clawt_fed_stamp_copy(stamp);
        const gchar *b = body;
        const gchar *bind = binding;
        const gchar *to = p.b_id;
        const guint8 *key = clawt_fleet_key_get_public(p.a);
        g_autoptr(GError) e = NULL;

        switch (i) {
        case 0: g_free(bad->kind); bad->kind = g_strdup("task.create"); break;
        case 1: g_free(bad->from); bad->from = g_strdup(p.b_id); break;
        case 2: to = p.a_id; break;                      /* replayed at another fleet */
        case 3: bad->time += 1; break;
        case 4: g_free(bad->nonce); bad->nonce = g_strdup("other"); break;
        case 5: b = "{\"text\":\"hellO\"}"; break;       /* body changed */
        case 6: bind = "POST /fed/v1/invite/accept"; break;
        case 7: key = clawt_fleet_key_get_public(p.b); break; /* wrong signer */
        }
        g_assert_false(clawt_fed_stamp_check(bad, key, to, b, bind, now, 300, &e));
        g_assert_error(e, CLAWT_ERROR, CLAWT_ERROR_AUTH);
    }

    /* Outside the window either way. */
    g_assert_false(clawt_fed_stamp_check(stamp, clawt_fleet_key_get_public(p.a),
                                         p.b_id, body, binding, now + 301, 300, NULL));
    g_assert_false(clawt_fed_stamp_check(stamp, clawt_fleet_key_get_public(p.a),
                                         p.b_id, body, binding, now - 301, 300, NULL));
    pair_clear(&p);
}

/* JSON, headers and the envelope object all carry a stamp unchanged. */
static const gchar *
lookup_header(const gchar *name, gpointer user_data)
{
    return g_hash_table_lookup(user_data, name);
}

static void
put_header(const gchar *name, const gchar *value, gpointer user_data)
{
    g_hash_table_insert(user_data, g_strdup(name), g_strdup(value));
}

static void
test_round_trips(void)
{
    Pair p;
    g_autoptr(ClawtFedStamp) stamp = NULL;
    g_autoptr(ClawtFedStamp) via_json = NULL;
    g_autoptr(ClawtFedStamp) via_headers = NULL;
    g_autoptr(ClawtFedStamp) via_envelope = NULL;
    g_autoptr(JsonObject) obj = NULL;
    g_autoptr(JsonObject) env = NULL;
    g_autoptr(GHashTable) headers = NULL;
    g_autofree gchar *body = NULL;
    g_autoptr(GError) error = NULL;

    pair_init(&p);
    stamp = clawt_fed_stamp_sign(p.a, 2, "hello", p.a_id, p.b_id, "{}", NULL, NULL);
    obj = clawt_fed_stamp_to_json(stamp);
    via_json = clawt_fed_stamp_from_json(obj, &error);
    g_assert_no_error(error);
    g_assert_true(clawt_fed_stamp_check(via_json, clawt_fleet_key_get_public(p.a),
                                        p.b_id, "{}", NULL, stamp->time, 300, NULL));
    g_assert_cmpuint(via_json->key_index, ==, 2);

    headers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    clawt_fed_stamp_to_headers(stamp, put_header, headers);
    via_headers = clawt_fed_stamp_from_headers(lookup_header, headers, &error);
    g_assert_no_error(error);
    g_assert_true(clawt_fed_stamp_check(via_headers, clawt_fleet_key_get_public(p.a),
                                        p.b_id, "{}", NULL, stamp->time, 300, NULL));

    env = clawt_fed_envelope_build(stamp, "{}");
    via_envelope = clawt_fed_envelope_split(env, &body, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(body, ==, "{}");

    /* A newline smuggled into a member is refused before verification. */
    g_hash_table_insert(headers, g_strdup("X-Clawt-Fed-Kind"), g_strdup("hello\nx"));
    g_assert_null(clawt_fed_stamp_from_headers(lookup_header, headers, NULL));
    pair_clear(&p);
}

/* Once per nonce, a restart included; the table stays bounded. */
static void
test_replay_guard(void)
{
    g_autoptr(ClawtFedReplayGuard) guard = clawt_fed_replay_guard_new(300);
    g_autoptr(ClawtFedReplayGuard) after = clawt_fed_replay_guard_new(300);
    g_autofree gchar *dir = g_dir_make_tmp("clawt-fed-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(dir, "nonces", NULL);
    gint64 now = 1700000000;
    guint i;

    g_assert_true(clawt_fed_replay_guard_admit(guard, "fleeta", "n1", now, now));
    g_assert_false(clawt_fed_replay_guard_admit(guard, "fleeta", "n1", now, now));
    /* Keyed per peer: the same nonce from another fleet is its own. */
    g_assert_true(clawt_fed_replay_guard_admit(guard, "fleetb", "n1", now, now));
    g_assert_false(clawt_fed_replay_guard_admit(guard, "fleeta", "n2", now - 400, now));

    g_assert_true(clawt_fed_replay_guard_save(guard, path, now, NULL));
    clawt_fed_replay_guard_load(after, path, now + 10);
    g_assert_false(clawt_fed_replay_guard_admit(after, "fleeta", "n1", now, now + 10));
    g_assert_false(clawt_fed_replay_guard_admit(after, "fleetb", "n1", now, now + 10));

    for (i = 0; i < CLAWT_FED_NONCES_PER_PEER + 50; i++) {
        g_autofree gchar *n = g_strdup_printf("bulk%u", i);
        g_assert_true(clawt_fed_replay_guard_admit(guard, "fleetc", n, now, now));
    }
    clawt_test_remove_tree(dir);
}

/* Sealed for B by A: B opens it, a third fleet cannot, tampering fails. */
static void
test_seal(void)
{
    Pair p;
    g_autoptr(ClawtFleetKey) c = clawt_fleet_key_generate(NULL);
    g_autofree gchar *sealed = NULL;
    g_autofree gchar *opened = NULL;
    g_autofree gchar *plain = NULL;
    g_autoptr(GError) error = NULL;
    const gchar *body = "{\"text\":\"secret plans\"}";
    gchar *at;

    pair_init(&p);
    sealed = clawt_fed_seal(p.a, clawt_fleet_key_get_seal_public(p.b), body, &error);
    g_assert_no_error(error);
    g_assert_true(clawt_fed_body_is_sealed(sealed));
    g_assert_null(strstr(sealed, "secret"));

    opened = clawt_fed_unseal(p.b, clawt_fleet_key_get_seal_public(p.a), sealed, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(opened, ==, body);

    g_assert_null(clawt_fed_unseal(c, clawt_fleet_key_get_seal_public(p.a), sealed, NULL));

    at = strstr(sealed, "\":\"") + 10;
    *at = (*at == 'A') ? 'B' : 'A';
    g_assert_null(clawt_fed_unseal(p.b, clawt_fleet_key_get_seal_public(p.a), sealed, NULL));

    /* An unsealed body passes through unchanged. */
    plain = clawt_fed_unseal(p.b, clawt_fleet_key_get_seal_public(p.a), body, NULL);
    g_assert_cmpstr(plain, ==, body);
    g_assert_false(clawt_fed_body_is_sealed(body));
    pair_clear(&p);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/fed-crypto/rfc8032-vector", test_rfc8032_vector);
    g_test_add_func("/fed-crypto/load-or-create", test_load_or_create);
    g_test_add_func("/fed-crypto/tamper-each-member", test_tamper_each_member);
    g_test_add_func("/fed-crypto/round-trips", test_round_trips);
    g_test_add_func("/fed-crypto/replay-guard", test_replay_guard);
    g_test_add_func("/fed-crypto/seal", test_seal);

    return g_test_run();
}
