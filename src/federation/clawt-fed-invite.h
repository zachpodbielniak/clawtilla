/*
 * clawt-fed-invite.h - One string that links two fleets
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Linking a fleet is one thing to send and one thing to paste.  An
 * invite holds where to reach the inviting fleet, its fingerprint (so
 * the accepting side checks it is talking to the fleet that invited it,
 * automatically), a one-use secret, the tier the inviter proposes, and
 * names for the preview.  Two spellings:
 *
 *   clawtilla://federate?v=1&u=https%3A%2F%2Ffleet.example.com&fp=...
 *   clawt-fed1-<base64url of the same query>-<checksum>
 *
 * The second survives chat apps that mangle URLs, and its checksum turns
 * a one-character typo into "this invite is damaged" instead of an
 * authentication failure three steps later.
 *
 * An invite is a secret until it is used or expires: whoever holds it
 * can join as the proposed tier (or ask to, when the inviter wants to
 * confirm).  It may also carry a Cloudflare Access service token for the
 * receiving fleet to send with its requests.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include "federation/clawt-fed-grant.h"

G_BEGIN_DECLS

/**
 * CLAWT_FED_INVITE_VERSION:
 *
 * The `v` an invite carries.  A reader refuses any other.
 */
#define CLAWT_FED_INVITE_VERSION (1)

/**
 * CLAWT_FED_INVITE_PREFIX:
 *
 * How the paste form begins.
 */
#define CLAWT_FED_INVITE_PREFIX "clawt-fed1-"

/**
 * CLAWT_FED_INVITE_MAX_LENGTH:
 *
 * The longest invite a reader accepts.  Far above any real one; it keeps
 * a pasted novel from reaching the parser.
 */
#define CLAWT_FED_INVITE_MAX_LENGTH (4096)

#define CLAWT_TYPE_FED_INVITE (clawt_fed_invite_get_type())

/**
 * ClawtFedInvite:
 * @endpoint: where to reach the inviter: `https://...`, `http://host:port`,
 *   `clawtilla://host:port` (the daemon's own IPC port) or `unix:/path`
 * @fleet_id: the inviter's fingerprint
 * @invite_id: which invite this is, so the inviter finds its secret
 * @secret: the one-use secret
 * @tier: the tier the inviter proposes for *itself* at the accepting side
 *   is not the inviter's call -- this is the tier it will give the
 *   accepter
 * @name: (nullable): the inviting fleet's display name
 * @owner: (nullable): who runs it
 * @expires: unix seconds after which the inviter refuses it
 * @access_id: (nullable): a Cloudflare Access service token id to send
 * @access_secret: (nullable): its secret
 * @tls_pin: (nullable): SHA-256 of the inviter's IPC certificate, for a
 *   `clawtilla://` endpoint that speaks TLS with a self-signed one
 *
 * An invite, parsed.
 */
typedef struct {
    gchar        *endpoint;
    gchar        *fleet_id;
    gchar        *invite_id;
    gchar        *secret;
    ClawtFedTier  tier;
    gchar        *name;
    gchar        *owner;
    gint64        expires;
    gchar        *access_id;
    gchar        *access_secret;
    gchar        *tls_pin;
} ClawtFedInvite;

GType clawt_fed_invite_get_type(void) G_GNUC_CONST;

/**
 * clawt_fed_invite_copy:
 * @invite: a #ClawtFedInvite
 *
 * Returns: (transfer full): a deep copy
 */
ClawtFedInvite *clawt_fed_invite_copy(const ClawtFedInvite *invite);

/**
 * clawt_fed_invite_free:
 * @invite: (transfer full) (nullable): a #ClawtFedInvite
 *
 * Frees it, wiping the secrets first.
 */
void clawt_fed_invite_free(ClawtFedInvite *invite);

/**
 * clawt_fed_invite_to_uri:
 * @invite: a #ClawtFedInvite
 *
 * Returns: (transfer full): `clawtilla://federate?...`
 */
gchar *clawt_fed_invite_to_uri(const ClawtFedInvite *invite);

/**
 * clawt_fed_invite_to_paste:
 * @invite: a #ClawtFedInvite
 *
 * Returns: (transfer full): `clawt-fed1-...`
 */
gchar *clawt_fed_invite_to_paste(const ClawtFedInvite *invite);

/**
 * clawt_fed_invite_parse:
 * @text: either spelling, with surrounding whitespace allowed
 * @error: (out) (optional): return location for a #GError
 *
 * Refuses another scheme, another version, a damaged paste string, an
 * endpoint that is not one of the four forms, a fingerprint of the wrong
 * length, and a missing secret.  Does not check expiry -- the preview
 * shows an expired invite as expired, which is more useful than refusing
 * to read it.
 *
 * Returns: (transfer full) (nullable): the invite
 */
ClawtFedInvite *clawt_fed_invite_parse(const gchar  *text,
                                       GError      **error);

/**
 * clawt_fed_endpoint_check:
 * @endpoint: an endpoint
 * @error: (out) (optional): return location for a #GError
 *
 * Whether @endpoint is one of `https://HOST[:PORT][/PATH]`,
 * `http://HOST:PORT`, `clawtilla://HOST[:PORT]` or `unix:/ABSOLUTE`.
 * Says nothing about whether plain text is allowed to it; that is
 * clawt_fed_transport_check().
 *
 * Returns: %TRUE if well formed
 */
gboolean clawt_fed_endpoint_check(const gchar  *endpoint,
                                  GError      **error);

/**
 * clawt_fed_proof:
 * @secret: the invite's secret
 * @accepter_key: the accepting fleet's public key, base64url
 * @inviter_fleet: the inviter's fleet id
 * @time: the accept stamp's time
 *
 * What the accepting fleet sends to prove it holds the invite without
 * sending the secret itself: HMAC-SHA256(secret, key | fleet | time).
 *
 * Returns: (transfer full): lowercase hex
 */
gchar *clawt_fed_proof(const gchar *secret,
                       const gchar *accepter_key,
                       const gchar *inviter_fleet,
                       gint64       time);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtFedInvite, clawt_fed_invite_free)

G_END_DECLS
