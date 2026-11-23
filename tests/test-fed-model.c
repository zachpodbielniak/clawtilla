/*
 * test-fed-model.c - Invites, tiers, grants, selectors, the registry and
 * the transport rules
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Everything here is pure, so it is tested without a daemon:
 *  - an invite survives both spellings, and a one-character typo in the
 *    paste form is "damaged", not an authentication failure later;
 *  - every tier has a value for every grant field (walking the enums, so
 *    a new tier or field without a row fails here), the presets keep
 *    their promises, rules layer in order, overrides win, an agent's own
 *    list only narrows, and a peer can never get fleet_admin;
 *  - selectors match what they say and refuse what is malformed;
 *  - the registry round-trips through its YAML, refuses a second record
 *    for one fleet, and honours key rotation's grace period;
 *  - plain text is allowed to this machine and a tailnet, to a LAN only
 *    when the operator says so, and never across the internet.
 */

#include "clawtilla.h"
#include "clawt-test-util.h"
#include <string.h>

static ClawtFedInvite *
sample_invite(void)
{
    ClawtFedInvite *invite = g_new0(ClawtFedInvite, 1);

    invite->endpoint = g_strdup("https://fleet.example.com");
    invite->fleet_id = g_strdup("abcdefghijklmnopqrstuvwxyz");
    invite->invite_id = g_strdup("inv1");
    invite->secret = g_strdup("0123456789abcdef0123");
    invite->tier = CLAWT_FED_TIER_TRUSTED;
    invite->name = g_strdup("Lab & Co");
    invite->owner = g_strdup("Alice");
    invite->expires = 1900000000;
    return invite;
}

static void
test_invite_round_trip(void)
{
    g_autoptr(ClawtFedInvite) invite = sample_invite();
    g_autofree gchar *uri = clawt_fed_invite_to_uri(invite);
    g_autofree gchar *paste = clawt_fed_invite_to_paste(invite);
    g_autoptr(ClawtFedInvite) a = NULL;
    g_autoptr(ClawtFedInvite) b = NULL;
    g_autoptr(GError) error = NULL;

    g_assert_true(g_str_has_prefix(uri, "clawtilla://federate?v=1&"));
    g_assert_true(g_str_has_prefix(paste, CLAWT_FED_INVITE_PREFIX));

    a = clawt_fed_invite_parse(uri, &error);
    g_assert_no_error(error);
    b = clawt_fed_invite_parse(paste, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(a->name, ==, "Lab & Co");
    g_assert_cmpstr(b->endpoint, ==, "https://fleet.example.com");
    g_assert_cmpint(b->tier, ==, CLAWT_FED_TIER_TRUSTED);
    g_assert_cmpint(b->expires, ==, 1900000000);
}

static void
test_invite_damage(void)
{
    g_autoptr(ClawtFedInvite) invite = sample_invite();
    g_autofree gchar *paste = clawt_fed_invite_to_paste(invite);
    g_autofree gchar *damaged = g_strdup(paste);
    g_autofree gchar *v2 = NULL;
    g_autofree gchar *uri = NULL;
    g_autoptr(GError) error = NULL;
    gsize mid = strlen(damaged) / 2;

    damaged[mid] = damaged[mid] == 'A' ? 'B' : 'A';
    g_assert_null(clawt_fed_invite_parse(damaged, &error));
    g_assert_nonnull(strstr(error->message, "damaged"));
    g_clear_error(&error);

    uri = clawt_fed_invite_to_uri(invite);
    v2 = g_strdup(uri);
    v2[strlen("clawtilla://federate?v=")] = '2';
    g_assert_null(clawt_fed_invite_parse(v2, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED);
    g_clear_error(&error);

    g_assert_null(clawt_fed_invite_parse("hello", &error));
    g_clear_error(&error);
    {
        g_autofree gchar *huge = g_strnfill(CLAWT_FED_INVITE_MAX_LENGTH + 1, 'a');
        g_assert_null(clawt_fed_invite_parse(huge, NULL));
    }
}

static void
test_endpoints(void)
{
    const gchar *good[] = {
        "https://fleet.example.com", "https://fleet.example.com:8443/base",
        "http://100.64.1.2:7711", "clawtilla://100.64.1.2:8792",
        "clawtilla://box.tailnet.ts.net", "clawtilla://[fd7a:115c:a1e0::1]:8792",
        "unix:/run/user/1000/clawtilla/federation.sock", NULL
    };
    const gchar *bad[] = {
        "", "ftp://x", "unix:relative", "https://user@host", "clawtilla://:80",
        "clawtilla://host:99999", "https://host/?q=1", NULL
    };
    guint i;

    for (i = 0; good[i] != NULL; i++)
        g_assert_true(clawt_fed_endpoint_check(good[i], NULL));
    for (i = 0; bad[i] != NULL; i++)
        g_assert_false(clawt_fed_endpoint_check(bad[i], NULL));
}

/* Plain text: this machine and a tailnet yes, LAN only by choice, internet never. */
static void
test_transport_rules(void)
{
    ClawtFedPath path;
    g_autoptr(GError) error = NULL;

    g_assert_true(clawt_fed_transport_check("https://x.example.com", FALSE, FALSE, &path, NULL));
    g_assert_cmpint(path, ==, CLAWT_FED_PATH_TLS);
    g_assert_true(clawt_fed_transport_check("unix:/tmp/x.sock", FALSE, FALSE, &path, NULL));
    g_assert_cmpint(path, ==, CLAWT_FED_PATH_LOCAL);
    g_assert_true(clawt_fed_transport_check("clawtilla://127.0.0.1:1", FALSE, FALSE, &path, NULL));
    g_assert_cmpint(path, ==, CLAWT_FED_PATH_LOCAL);

    /* Both edges of 100.64.0.0/10, the v6 prefix and a MagicDNS name. */
    g_assert_true(clawt_fed_transport_check("http://100.64.0.1:7711", FALSE, FALSE, &path, NULL));
    g_assert_cmpint(path, ==, CLAWT_FED_PATH_TAILNET);
    g_assert_true(clawt_fed_transport_check("http://100.127.255.254:7711", FALSE, FALSE, &path, NULL));
    g_assert_false(clawt_fed_transport_check("http://100.128.0.1:7711", FALSE, FALSE, &path, NULL));
    g_assert_true(clawt_fed_transport_check("clawtilla://[fd7a:115c:a1e0::5]:8792", FALSE, FALSE, &path, NULL));
    g_assert_cmpint(path, ==, CLAWT_FED_PATH_TAILNET);
    g_assert_true(clawt_fed_transport_check("clawtilla://box.tail1234.ts.net", FALSE, FALSE, &path, NULL));

    /* A LAN needs the operator's word, and is then sealed under auto. */
    g_assert_false(clawt_fed_transport_check("http://192.168.1.5:7711", FALSE, FALSE, &path, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED);
    g_assert_nonnull(strstr(error->message, "federation.allow_lan_plaintext"));
    g_clear_error(&error);
    g_assert_true(clawt_fed_transport_check("http://192.168.1.5:7711", TRUE, FALSE, &path, NULL));
    g_assert_cmpint(path, ==, CLAWT_FED_PATH_LAN);
    g_assert_true(clawt_fed_should_seal(CLAWT_FED_SEAL_AUTO, path));
    g_assert_false(clawt_fed_should_seal(CLAWT_FED_SEAL_AUTO, CLAWT_FED_PATH_TAILNET));
    g_assert_true(clawt_fed_should_seal(CLAWT_FED_SEAL_ALWAYS, CLAWT_FED_PATH_TLS));
    g_assert_false(clawt_fed_transport_check("clawtilla://nas.lan", FALSE, FALSE, &path, NULL));

    /* The internet, never in plain text -- not even with the LAN switch. */
    g_assert_false(clawt_fed_transport_check("http://8.8.8.8:7711", TRUE, FALSE, &path, &error));
    g_assert_nonnull(strstr(error->message, "https://"));
    /* ... unless the IPC listener has a certificate. */
    g_assert_true(clawt_fed_transport_check("clawtilla://8.8.8.8:8792", FALSE, TRUE, &path, NULL));
    g_assert_cmpint(path, ==, CLAWT_FED_PATH_TLS);
}

/* Every tier has a value for every field, and the enum walk covers them. */
static void
test_tier_matrix(void)
{
    guint t;
    guint f;

    g_assert_cmpuint(clawt_fed_tier_count(), ==, 4);
    for (t = 0; t < clawt_fed_tier_count(); t++) {
        ClawtFedTier tier = clawt_fed_tier_nth(t);
        g_autoptr(ClawtFedGrant) grant = clawt_fed_grant_preset(tier);
        ClawtFedTier back;

        g_assert_true(clawt_fed_tier_from_nick(clawt_fed_tier_nick(tier), &back));
        g_assert_cmpint(back, ==, tier);
        g_assert_cmpstr(clawt_fed_tier_tone(tier), !=, "neutral");
        g_assert_cmpuint(strlen(clawt_fed_tier_summary(tier)), >, 10);
        for (f = 0; f < clawt_fed_grant_field_count(); f++) {
            g_autofree gchar *value = clawt_fed_grant_get(grant, f);
            g_autoptr(ClawtFedGrant) copy = clawt_fed_grant_copy(grant);
            g_assert_nonnull(value);
            /* What get says, set reads back. */
            g_assert_true(clawt_fed_grant_set(copy, clawt_fed_grant_field_name(f), value, NULL));
            g_assert_nonnull(clawt_fed_grant_field_doc(f));
        }
    }
    /* One cap field per action class, named from the enum. */
    g_assert_cmpint(clawt_fed_grant_field_find("cap.exec"), >=, 0);
    g_assert_cmpint(clawt_fed_grant_field_find("cap.fleet_admin"), >=, 0);
    g_assert_cmpint(clawt_fed_grant_field_find("cap.bogus"), ==, -1);
}

static void
test_presets_keep_promises(void)
{
    g_autoptr(ClawtFedGrant) owned = clawt_fed_grant_preset(CLAWT_FED_TIER_OWNED);
    g_autoptr(ClawtFedGrant) trusted = clawt_fed_grant_preset(CLAWT_FED_TIER_TRUSTED);
    g_autoptr(ClawtFedGrant) limited = clawt_fed_grant_preset(CLAWT_FED_TIER_LIMITED);
    g_autoptr(ClawtFedGrant) untrusted = clawt_fed_grant_preset(CLAWT_FED_TIER_UNTRUSTED);

    g_assert_cmpint(owned->reach, ==, CLAWT_FED_REACH_ALL);
    g_assert_cmpint(owned->transcripts, ==, CLAWT_FED_TRANSCRIPTS_REACHABLE);
    g_assert_cmpint(trusted->caps[CLAWT_ACTION_EXEC], ==, CLAWT_POLICY_ASK);
    g_assert_cmpint(trusted->caps[CLAWT_ACTION_WRITE], ==, CLAWT_POLICY_ASK);
    g_assert_cmpint(limited->reach, ==, CLAWT_FED_REACH_LISTED);
    g_assert_cmpint(limited->delegate, ==, CLAWT_POLICY_ASK);
    g_assert_cmpint(limited->caps[CLAWT_ACTION_EXEC], ==, CLAWT_POLICY_DENY);
    g_assert_cmpint(untrusted->reach, ==, CLAWT_FED_REACH_INBOX);
    g_assert_cmpint(untrusted->message, ==, CLAWT_POLICY_REVIEW);
    g_assert_cmpint(untrusted->delegate, ==, CLAWT_POLICY_DENY);
}

static JsonObject *
peer_json(const gchar *alias, const gchar *tier, const gchar *tag)
{
    JsonObject *o = json_object_new();
    JsonArray *tags = json_array_new();

    json_object_set_string_member(o, "alias", alias);
    json_object_set_string_member(o, "tier", tier);
    json_object_set_string_member(o, "owner", "Alice Smith");
    json_object_set_string_member(o, "health", "live");
    if (tag != NULL)
        json_array_add_string_element(tags, tag);
    json_object_set_array_member(o, "tags", tags);
    return o;
}

/* Rules in order, overrides last, problems reported not half-applied. */
static void
test_resolve_layers(void)
{
    gchar *rules[] = {
        "tag:acme: cap.exec=allow; messages_per_hour=5",
        "tag:acme+tier:trusted: messages_per_hour=7",
        "tag:other: cap.exec=allow",
        "nonsense",
        "tag:acme: budget_cents_per_day=lots",
        "tag:acme: cap.fleet_admin=allow",
        NULL
    };
    g_autoptr(JsonObject) peer = peer_json("lab", "trusted", "acme");
    g_autoptr(JsonObject) overrides = json_object_new();
    g_autoptr(GHashTable) explain = NULL;
    g_autoptr(GPtrArray) problems = NULL;
    g_autoptr(ClawtFedGrant) grant = NULL;

    json_object_set_string_member(overrides, "delegate", "deny");
    grant = clawt_fed_grant_resolve(CLAWT_FED_TIER_TRUSTED, rules, peer, overrides,
                                    &explain, &problems);
    g_assert_cmpint(grant->caps[CLAWT_ACTION_EXEC], ==, CLAWT_POLICY_ALLOW);
    g_assert_cmpint(grant->messages_per_hour, ==, 7);
    g_assert_cmpint(grant->delegate, ==, CLAWT_POLICY_DENY);
    /* The bad budget rule did not apply, and fleet_admin never opens. */
    g_assert_cmpint(grant->budget_cents_per_day, ==, 2000);
    g_assert_cmpint(grant->caps[CLAWT_ACTION_FLEET_ADMIN], ==, CLAWT_POLICY_DENY);
    g_assert_cmpuint(problems->len, ==, 2);
    g_assert_cmpstr(g_hash_table_lookup(explain, "cap.exec"), ==, "rule 1: tag:acme");
    g_assert_cmpstr(g_hash_table_lookup(explain, "messages_per_hour"), ==, "rule 2: tag:acme+tier:trusted");
    g_assert_cmpstr(g_hash_table_lookup(explain, "delegate"), ==, "override");
    g_assert_cmpstr(g_hash_table_lookup(explain, "reach"), ==, "tier trusted");
}

/* Reach: listed and inbox; the agent's own list only narrows. */
static void
test_reach(void)
{
    g_autoptr(ClawtFedGrant) limited = clawt_fed_grant_preset(CLAWT_FED_TIER_LIMITED);
    g_autoptr(ClawtFedGrant) untrusted = clawt_fed_grant_preset(CLAWT_FED_TIER_UNTRUSTED);
    g_autoptr(ClawtFedGrant) owned = clawt_fed_grant_preset(CLAWT_FED_TIER_OWNED);
    g_autoptr(JsonObject) lab = peer_json("lab", "owned", "acme");
    gchar *tags[] = { "frontend", NULL };
    gchar *only_acme[] = { "tag:acme", NULL };
    gchar *only_other[] = { "tag:other", NULL };

    g_assert_true(clawt_fed_grant_set(limited, "agents", "atlas, team:web", NULL));
    g_assert_true(clawt_fed_grant_reaches(limited, "atlas", NULL, NULL, "chief", NULL, lab));
    g_assert_true(clawt_fed_grant_reaches(limited, "kudu", "web", NULL, "chief", NULL, lab));
    g_assert_false(clawt_fed_grant_reaches(limited, "oryx", "ops", NULL, "chief", NULL, lab));
    g_assert_true(clawt_fed_grant_reaches(limited, "chief", NULL, NULL, "chief", NULL, lab));

    g_assert_true(clawt_fed_grant_reaches(untrusted, "chief", NULL, NULL, "chief", NULL, lab));
    g_assert_false(clawt_fed_grant_reaches(untrusted, "atlas", NULL, NULL, "chief", NULL, lab));

    g_assert_true(clawt_fed_grant_reaches(owned, "oryx", "ops", tags, "chief", only_acme, lab));
    g_assert_false(clawt_fed_grant_reaches(owned, "oryx", "ops", tags, "chief", only_other, lab));
    /* Narrowing cannot widen: an agent listing a peer the grant shuts out. */
    g_assert_false(clawt_fed_grant_reaches(untrusted, "atlas", NULL, NULL, "chief", only_acme, lab));

    g_assert_false(clawt_fed_grant_set(limited, "agents", "team:", NULL));
    g_assert_false(clawt_fed_grant_set(limited, "reach", "most", NULL));
    g_assert_false(clawt_fed_grant_set(limited, "messages_per_hour", "-1", NULL));
    g_assert_cmpint(clawt_fed_policy_stricter(CLAWT_POLICY_ALLOW, CLAWT_POLICY_ASK), ==, CLAWT_POLICY_ASK);
    g_assert_cmpint(clawt_fed_policy_stricter(CLAWT_POLICY_DENY, CLAWT_POLICY_REVIEW), ==, CLAWT_POLICY_DENY);
}

static void
test_selectors(void)
{
    g_autoptr(JsonObject) lab = peer_json("lab", "trusted", "client-acme");
    g_autoptr(JsonObject) home = peer_json("home", "owned", NULL);
    gchar *web_tags[] = { "frontend", NULL };

    g_assert_true(clawt_fed_selector_matches("*", lab));
    g_assert_true(clawt_fed_selector_matches("tag:client-acme", lab));
    g_assert_true(clawt_fed_selector_matches("TAG:Client-Acme", lab));
    g_assert_false(clawt_fed_selector_matches("tag:client-acme", home));
    g_assert_true(clawt_fed_selector_matches("tier:trusted+online", lab));
    g_assert_true(clawt_fed_selector_matches("owner:alice*", lab));
    g_assert_true(clawt_fed_selector_matches("!tier:owned", lab));
    g_assert_false(clawt_fed_selector_matches("!tier:owned", home));
    g_assert_true(clawt_fed_selector_matches("tag:nope,name:ho*", home));
    g_assert_false(clawt_fed_selector_matches("tier:bestest", lab));

    g_assert_false(clawt_fed_selector_check("tier:bestest", NULL));
    g_assert_false(clawt_fed_selector_check("colour:red", NULL));
    g_assert_false(clawt_fed_selector_check("tag:", NULL));
    g_assert_false(clawt_fed_selector_check("a++b", NULL));
    g_assert_true(clawt_fed_selector_check("tier:limited+!online,tag:x", NULL));

    g_assert_true(clawt_fed_agent_selector_matches("at*", "atlas", NULL, NULL));
    g_assert_true(clawt_fed_agent_selector_matches("tag:frontend", "kudu", "web", web_tags));
    g_assert_false(clawt_fed_agent_selector_matches("team:ops", "kudu", "web", web_tags));
    g_assert_false(clawt_fed_agent_selector_check("owner:x", NULL));
}

static void
test_registry(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-fed-XXXXXX", NULL);
    g_autofree gchar *peers = g_build_filename(dir, "state", "federation", "peers.yaml", NULL);
    g_autofree gchar *invites = g_build_filename(dir, "secrets", "federation", "invites.yaml", NULL);
    g_autoptr(ClawtFedRegistry) reg = clawt_fed_registry_new(peers, invites);
    g_autoptr(ClawtFedRegistry) again = clawt_fed_registry_new(peers, invites);
    g_autoptr(ClawtFleetKey) k1 = clawt_fleet_key_generate(NULL);
    g_autoptr(ClawtFleetKey) k2 = clawt_fleet_key_generate(NULL);
    g_autofree gchar *pub1 = clawt_fleet_key_public_text(k1);
    g_autofree gchar *pub2 = clawt_fleet_key_public_text(k2);
    g_autofree gchar *fleet1 = clawt_fleet_key_fleet_id(k1);
    g_autoptr(GError) error = NULL;
    ClawtFedPeer *peer;
    ClawtFedPeer *loaded;
    ClawtFedPendingInvite *inv;
    guint8 raw[32];
    g_autoptr(GPtrArray) list = NULL;
    GStatBuf st;

    g_assert_true(clawt_fed_registry_load(reg, &error));
    g_assert_no_error(error);

    peer = clawt_fed_peer_new("lab", fleet1, pub1);
    peer->tier = CLAWT_FED_TIER_TRUSTED;
    peer->label = g_strdup("Lab");
    g_strfreev(peer->tags);
    peer->tags = g_strsplit("client-acme,gpu", ",", -1);
    peer->overrides = json_object_new();
    json_object_set_string_member(peer->overrides, "cap.exec", "allow");
    peer->endpoint = g_strdup("clawtilla://100.64.0.9:8792");
    peer->hidden = TRUE;
    g_assert_true(clawt_fed_registry_add(reg, peer, &error));

    /* The same fleet under another alias is refused. */
    g_assert_false(clawt_fed_registry_add(reg, clawt_fed_peer_new("lab2", fleet1, pub1), &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_ALREADY_EXISTS);
    g_clear_error(&error);
    g_assert_false(clawt_fed_registry_add(reg, clawt_fed_peer_new("Bad Alias", "x", pub1), NULL));

    inv = g_new0(ClawtFedPendingInvite, 1);
    inv->id = g_strdup("i1");
    inv->secret = g_strdup("sssssssssssssssssssss");
    inv->tier = CLAWT_FED_TIER_LIMITED;
    inv->expires = 100;
    clawt_fed_registry_add_invite(reg, inv);

    g_assert_true(clawt_fed_registry_save(reg, &error));
    g_assert_no_error(error);
    g_assert_cmpint(g_stat(peers, &st), ==, 0);
    g_assert_cmpint(st.st_mode & 0777, ==, 0600);
    g_assert_cmpint(g_stat(invites, &st), ==, 0);
    g_assert_cmpint(st.st_mode & 0777, ==, 0600);

    g_assert_true(clawt_fed_registry_load(again, &error));
    loaded = clawt_fed_registry_get(again, "lab");
    g_assert_nonnull(loaded);
    g_assert_cmpint(loaded->tier, ==, CLAWT_FED_TIER_TRUSTED);
    g_assert_true(loaded->hidden);
    g_assert_cmpstr(loaded->tags[1], ==, "gpu");
    g_assert_cmpstr(json_object_get_string_member(loaded->overrides, "cap.exec"), ==, "allow");
    g_assert_cmpstr(loaded->endpoint, ==, "clawtilla://100.64.0.9:8792");
    g_assert_true(clawt_fed_registry_by_fleet(again, fleet1) == loaded);
    g_assert_nonnull(clawt_fed_registry_find_invite(again, "i1"));
    g_assert_cmpuint(clawt_fed_registry_prune_invites(again, 200), ==, 1);

    /* Rotation: the new key is current, the old one lasts the grace period. */
    g_assert_cmpuint(clawt_fed_peer_rotate(loaded, pub2, 1000), ==, 1);
    g_assert_true(clawt_fed_peer_public_key(loaded, 0, 1000 + CLAWT_FED_KEY_GRACE_SECONDS, raw));
    g_assert_false(clawt_fed_peer_public_key(loaded, 0, 1001 + CLAWT_FED_KEY_GRACE_SECONDS, raw));
    g_assert_true(clawt_fed_peer_public_key(loaded, 1, G_MAXINT64, raw));
    g_assert_false(clawt_fed_peer_public_key(loaded, 2, 0, raw));

    list = clawt_fed_registry_list(again);
    g_assert_cmpuint(list->len, ==, 1);
    g_assert_true(clawt_fed_registry_remove(again, "lab"));

    {
        g_autofree gchar *a = clawt_fed_alias_suggest("Bob's Lab!");
        g_autofree gchar *b = clawt_fed_alias_suggest("!!!");
        g_assert_cmpstr(a, ==, "bob-s-lab");
        g_assert_cmpstr(b, ==, "peer");
    }
    clawt_test_remove_tree(dir);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/fed-model/invite-round-trip", test_invite_round_trip);
    g_test_add_func("/fed-model/invite-damage", test_invite_damage);
    g_test_add_func("/fed-model/endpoints", test_endpoints);
    g_test_add_func("/fed-model/transport-rules", test_transport_rules);
    g_test_add_func("/fed-model/tier-matrix", test_tier_matrix);
    g_test_add_func("/fed-model/presets-keep-promises", test_presets_keep_promises);
    g_test_add_func("/fed-model/resolve-layers", test_resolve_layers);
    g_test_add_func("/fed-model/reach", test_reach);
    g_test_add_func("/fed-model/selectors", test_selectors);
    g_test_add_func("/fed-model/registry", test_registry);

    return g_test_run();
}
