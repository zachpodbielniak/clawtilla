/*
 * clawt-fed-invite.c - One string that links two fleets
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "federation/clawt-fed-invite.h"

#include <errno.h>
#include <string.h>

G_DEFINE_BOXED_TYPE(ClawtFedInvite, clawt_fed_invite,
                    clawt_fed_invite_copy, clawt_fed_invite_free)

ClawtFedInvite *
clawt_fed_invite_copy(
    const ClawtFedInvite *invite
){
    ClawtFedInvite *copy;

    copy = g_new0(ClawtFedInvite, 1);
    copy->endpoint = g_strdup(invite->endpoint);
    copy->fleet_id = g_strdup(invite->fleet_id);
    copy->invite_id = g_strdup(invite->invite_id);
    copy->secret = g_strdup(invite->secret);
    copy->tier = invite->tier;
    copy->name = g_strdup(invite->name);
    copy->owner = g_strdup(invite->owner);
    copy->expires = invite->expires;
    copy->access_id = g_strdup(invite->access_id);
    copy->access_secret = g_strdup(invite->access_secret);
    copy->tls_pin = g_strdup(invite->tls_pin);
    return copy;
}

static void
wipe(
    gchar *s
){
    if (s != NULL)
        memset(s, 0, strlen(s));
    g_free(s);
}

void
clawt_fed_invite_free(
    ClawtFedInvite *invite
){
    if (invite == NULL)
        return;
    g_free(invite->endpoint);
    g_free(invite->fleet_id);
    g_free(invite->invite_id);
    wipe(invite->secret);
    g_free(invite->name);
    g_free(invite->owner);
    wipe(invite->access_id);
    wipe(invite->access_secret);
    g_free(invite->tls_pin);
    g_free(invite);
}

static void
add_param(
    GString     *out,
    const gchar *key,
    const gchar *value
){
    g_autofree gchar *escaped = NULL;

    if (value == NULL || *value == '\0')
        return;
    escaped = g_uri_escape_string(value, NULL, FALSE);
    g_string_append_printf(out, "%s%s=%s", out->len > 0 ? "&" : "", key, escaped);
}

static gchar *
query_of(
    const ClawtFedInvite *invite
){
    GString *q;
    g_autofree gchar *v = NULL;
    g_autofree gchar *exp = NULL;

    q = g_string_new(NULL);
    v = g_strdup_printf("%d", CLAWT_FED_INVITE_VERSION);
    exp = g_strdup_printf("%" G_GINT64_FORMAT, invite->expires);
    add_param(q, "v", v);
    add_param(q, "u", invite->endpoint);
    add_param(q, "fp", invite->fleet_id);
    add_param(q, "i", invite->invite_id);
    add_param(q, "s", invite->secret);
    add_param(q, "t", clawt_fed_tier_nick(invite->tier));
    add_param(q, "n", invite->name);
    add_param(q, "o", invite->owner);
    add_param(q, "exp", exp);
    add_param(q, "ca", invite->access_id);
    add_param(q, "cs", invite->access_secret);
    add_param(q, "tp", invite->tls_pin);
    return g_string_free(q, FALSE);
}

gchar *
clawt_fed_invite_to_uri(
    const ClawtFedInvite *invite
){
    g_autofree gchar *q = query_of(invite);

    return g_strdup_printf("clawtilla://federate?%s", q);
}

/* Six base32 characters of SHA-256: a typo is caught 1 - 2^-30 times. */
static gchar *
checksum_of(
    const gchar *payload
){
    g_autofree gchar *hex = g_compute_checksum_for_string(G_CHECKSUM_SHA256, payload, -1);

    return g_strndup(hex, 6);
}

gchar *
clawt_fed_invite_to_paste(
    const ClawtFedInvite *invite
){
    g_autofree gchar *q = query_of(invite);
    g_autofree gchar *payload = clawt_fed_b64url_encode((const guint8 *)q, strlen(q));
    g_autofree gchar *sum = checksum_of(payload);

    return g_strdup_printf("%s%s-%s", CLAWT_FED_INVITE_PREFIX, payload, sum);
}

static gboolean
host_port_ok(
    const gchar *rest,
    gboolean     port_required
){
    const gchar *colon;
    const gchar *host_end;
    gchar *end = NULL;
    guint64 port;

    if (*rest == '\0')
        return FALSE;
    if (*rest == '[') {
        host_end = strchr(rest, ']');
        if (host_end == NULL || host_end == rest + 1)
            return FALSE;
        colon = host_end[1] == ':' ? host_end + 1 : NULL;
        if (host_end[1] != '\0' && colon == NULL)
            return FALSE;
    } else {
        colon = strchr(rest, ':');
        host_end = colon != NULL ? colon : rest + strlen(rest);
        if (host_end == rest)
            return FALSE;
    }
    if (colon == NULL)
        return !port_required;
    errno = 0;
    port = g_ascii_strtoull(colon + 1, &end, 10);
    return errno == 0 && end != colon + 1 && *end == '\0' && port >= 1 && port <= 65535;
}

gboolean
clawt_fed_endpoint_check(
    const gchar  *endpoint,
    GError      **error
){
    if (endpoint == NULL || *endpoint == '\0')
        goto bad;
    if (g_str_has_prefix(endpoint, "unix:")) {
        if (endpoint[5] == '/' && strlen(endpoint) > 6)
            return TRUE;
        goto bad;
    }
    if (g_str_has_prefix(endpoint, "clawtilla://")) {
        if (host_port_ok(endpoint + strlen("clawtilla://"), FALSE))
            return TRUE;
        goto bad;
    }
    if (g_str_has_prefix(endpoint, "http://") || g_str_has_prefix(endpoint, "https://")) {
        g_autoptr(GUri) uri = g_uri_parse(endpoint, G_URI_FLAGS_NONE, NULL);
        if (uri == NULL || g_uri_get_host(uri) == NULL || *g_uri_get_host(uri) == '\0'
            || g_uri_get_userinfo(uri) != NULL || g_uri_get_query(uri) != NULL
            || g_uri_get_fragment(uri) != NULL)
            goto bad;
        return TRUE;
    }

bad:
    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                "\"%s\" is not a federation endpoint: use https://HOST, "
                "http://HOST:PORT, clawtilla://HOST:PORT or unix:/PATH",
                endpoint != NULL ? endpoint : "");
    return FALSE;
}

static ClawtFedInvite *
parse_query(
    const gchar  *query,
    GError      **error
){
    g_autoptr(GHashTable) params = NULL;
    g_autoptr(ClawtFedInvite) invite = NULL;
    g_autoptr(GError) local = NULL;
    const gchar *v;
    const gchar *exp;
    gchar *end = NULL;

    params = g_uri_parse_params(query, -1, "&", G_URI_PARAMS_NONE, &local);
    if (params == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "this invite is damaged: %s", local->message);
        return NULL;
    }
    v = g_hash_table_lookup(params, "v");
    if (g_strcmp0(v, "1") != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                    "this invite is version %s; this clawtilla reads version %d",
                    v != NULL ? v : "(none)", CLAWT_FED_INVITE_VERSION);
        return NULL;
    }

    invite = g_new0(ClawtFedInvite, 1);
    invite->endpoint = g_strdup(g_hash_table_lookup(params, "u"));
    invite->fleet_id = g_strdup(g_hash_table_lookup(params, "fp"));
    invite->invite_id = g_strdup(g_hash_table_lookup(params, "i"));
    invite->secret = g_strdup(g_hash_table_lookup(params, "s"));
    invite->name = g_strdup(g_hash_table_lookup(params, "n"));
    invite->owner = g_strdup(g_hash_table_lookup(params, "o"));
    invite->access_id = g_strdup(g_hash_table_lookup(params, "ca"));
    invite->access_secret = g_strdup(g_hash_table_lookup(params, "cs"));
    invite->tls_pin = g_strdup(g_hash_table_lookup(params, "tp"));

    if (!clawt_fed_endpoint_check(invite->endpoint, error))
        return NULL;
    if (invite->fleet_id == NULL
        || strlen(invite->fleet_id) != CLAWT_FLEET_FINGERPRINT_LENGTH) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "this invite has no fleet fingerprint");
        return NULL;
    }
    if (invite->invite_id == NULL || *invite->invite_id == '\0'
        || invite->secret == NULL || strlen(invite->secret) < 16) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "this invite has no secret");
        return NULL;
    }
    if (!clawt_fed_tier_from_nick(g_hash_table_lookup(params, "t"), &invite->tier))
        invite->tier = CLAWT_FED_TIER_UNTRUSTED;
    exp = g_hash_table_lookup(params, "exp");
    invite->expires = exp != NULL ? g_ascii_strtoll(exp, &end, 10) : 0;
    if (exp == NULL || end == exp || *end != '\0') {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "this invite has no expiry");
        return NULL;
    }
    if ((invite->access_id == NULL) != (invite->access_secret == NULL)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "this invite has half a Cloudflare Access token");
        return NULL;
    }
    return g_steal_pointer(&invite);
}

ClawtFedInvite *
clawt_fed_invite_parse(
    const gchar  *text,
    GError      **error
){
    g_autofree gchar *trimmed = NULL;

    if (text == NULL || strlen(text) > CLAWT_FED_INVITE_MAX_LENGTH) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "that is not an invite");
        return NULL;
    }
    trimmed = g_strstrip(g_strdup(text));

    if (g_str_has_prefix(trimmed, "clawtilla://federate?"))
        return parse_query(trimmed + strlen("clawtilla://federate?"), error);

    if (g_str_has_prefix(trimmed, CLAWT_FED_INVITE_PREFIX)) {
        const gchar *payload = trimmed + strlen(CLAWT_FED_INVITE_PREFIX);
        const gchar *dash = strrchr(payload, '-');
        g_autofree gchar *body = NULL;
        g_autofree gchar *sum = NULL;
        g_autofree guint8 *query = NULL;
        g_autofree gchar *query_text = NULL;
        gsize length;

        if (dash == NULL || dash == payload) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                        "this invite is damaged (no checksum); copy it again");
            return NULL;
        }
        body = g_strndup(payload, dash - payload);
        sum = checksum_of(body);
        if (g_ascii_strcasecmp(sum, dash + 1) != 0) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                        "this invite is damaged (checksum mismatch: a character "
                        "was changed or lost); copy it again");
            return NULL;
        }
        query = clawt_fed_b64url_decode(body, &length);
        if (query == NULL) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                        "this invite is damaged; copy it again");
            return NULL;
        }
        query_text = g_strndup((const gchar *)query, length);
        return parse_query(query_text, error);
    }

    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                "that is not an invite: it should start with "
                "clawtilla://federate? or %s", CLAWT_FED_INVITE_PREFIX);
    return NULL;
}

gchar *
clawt_fed_proof(
    const gchar *secret,
    const gchar *accepter_key,
    const gchar *inviter_fleet,
    gint64       time
){
    g_autofree gchar *data = NULL;

    data = g_strdup_printf("%s|%s|%" G_GINT64_FORMAT, accepter_key, inviter_fleet, time);
    return g_compute_hmac_for_string(G_CHECKSUM_SHA256, (const guchar *)secret,
                                     strlen(secret), data, -1);
}
