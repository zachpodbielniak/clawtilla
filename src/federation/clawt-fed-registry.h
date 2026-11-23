/*
 * clawt-fed-registry.h - The fleets this one is linked with
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A peer record is daemon-owned state, not configuration a person edits
 * by hand: it holds the peer's keys (learned at accept, changed only by
 * a signed rotation), its tier, tags, overrides and display names.  It
 * lives in `<state_dir>/federation/peers.yaml`, written atomically 0600.
 * Global behaviour (`federation.*`) stays in clawtilla.yaml and the
 * schema.
 *
 * Outstanding invites hold a secret, so they live beside the other
 * secrets: `<secrets.dir>/federation/invites.yaml`.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>
#include "federation/clawt-fed-grant.h"

G_BEGIN_DECLS

/**
 * CLAWT_FED_ALIAS_MAX:
 *
 * The longest alias.  It appears in room ids and on badges.
 */
#define CLAWT_FED_ALIAS_MAX (32)

/**
 * CLAWT_FED_KEY_GRACE_SECONDS:
 *
 * How long a rotated-out key is still accepted: a day, so requests
 * already in flight and a peer that was offline at the rotation still
 * get through.
 */
#define CLAWT_FED_KEY_GRACE_SECONDS (24 * 3600)

/**
 * ClawtFedOnRevoke:
 * @CLAWT_FED_ON_REVOKE_CANCEL: cancel tasks the peer gave us
 * @CLAWT_FED_ON_REVOKE_FINISH: let them finish; the results go nowhere
 *
 * What happens to work in progress for a peer when the link is revoked.
 */
typedef enum {
    CLAWT_FED_ON_REVOKE_CANCEL = 0,
    CLAWT_FED_ON_REVOKE_FINISH
} ClawtFedOnRevoke;

#define CLAWT_TYPE_FED_PEER (clawt_fed_peer_get_type())

/**
 * ClawtFedPeer:
 * @alias: our name for it; a valid id, never changed (room ids use it)
 * @label: (nullable): its own display name
 * @fleet_id: its fingerprint
 * @keys: (element-type utf8): its public keys, base64url, by index
 * @key_until: (element-type gint64): per key, 0 while current, else the
 *   unix time after which it is no longer accepted
 * @seal_key: (nullable): its X25519 key, base64url
 * @endpoint: (nullable): where we reach it; %NULL when only it dials us
 * @tier: what we trust it with
 * @owner: (nullable): who runs it
 * @tags: (array zero-terminated=1): our tags for it
 * @overrides: (nullable): grant fields set for this peer alone
 * @hidden: kept out of every sidebar until shown
 * @muted: its alerts are quiet
 * @on_revoke: what happens to its work at revoke
 * @status: `pending` (waiting for our approval), `active` or `revoked`
 * @created_at: unix seconds
 * @their_grant: (nullable): what it reports allowing us, for display
 * @cert_pin: (nullable): SHA-256 of its TLS certificate, for a
 *   self-signed direct link
 * @version: (nullable): the clawtilla version it last reported
 * @access_id: (nullable): a Cloudflare Access token id to send it
 *
 * One linked fleet.  The Access token's secret is never in here; it is in
 * the secrets directory under the alias.
 */
typedef struct {
    gchar            *alias;
    gchar            *label;
    gchar            *fleet_id;
    GPtrArray        *keys;
    GArray           *key_until;
    gchar            *seal_key;
    gchar            *endpoint;
    ClawtFedTier      tier;
    gchar            *owner;
    gchar           **tags;
    JsonObject       *overrides;
    gboolean          hidden;
    gboolean          muted;
    ClawtFedOnRevoke  on_revoke;
    gchar            *status;
    gint64            created_at;
    JsonObject       *their_grant;
    gchar            *cert_pin;
    gchar            *version;
    gchar            *access_id;
} ClawtFedPeer;

GType clawt_fed_peer_get_type(void) G_GNUC_CONST;

/**
 * clawt_fed_peer_new:
 * @alias: our name for it
 * @fleet_id: its fingerprint
 * @public_key: its first public key, base64url
 *
 * Returns: (transfer full): an active peer at the untrusted tier
 */
ClawtFedPeer *clawt_fed_peer_new(const gchar *alias,
                                 const gchar *fleet_id,
                                 const gchar *public_key);

/**
 * clawt_fed_peer_copy:
 * @peer: a #ClawtFedPeer
 *
 * Returns: (transfer full): a deep copy
 */
ClawtFedPeer *clawt_fed_peer_copy(const ClawtFedPeer *peer);

/**
 * clawt_fed_peer_free:
 * @peer: (transfer full) (nullable): a #ClawtFedPeer
 */
void clawt_fed_peer_free(ClawtFedPeer *peer);

/**
 * clawt_fed_peer_public_key:
 * @peer: a #ClawtFedPeer
 * @index: a key index from a stamp
 * @now: unix seconds
 * @out_key: (out caller-allocates) (array fixed-size=32): the key
 *
 * Returns: %TRUE if @index names a key that is still accepted
 */
gboolean clawt_fed_peer_public_key(const ClawtFedPeer *peer,
                                   guint               index,
                                   gint64              now,
                                   guint8             *out_key);

/**
 * clawt_fed_peer_rotate:
 * @peer: a #ClawtFedPeer
 * @new_key: the new key, base64url
 * @now: unix seconds
 *
 * Adds @new_key as current, retiring the others after
 * %CLAWT_FED_KEY_GRACE_SECONDS.
 *
 * Returns: the new key's index
 */
guint clawt_fed_peer_rotate(ClawtFedPeer *peer,
                            const gchar  *new_key,
                            gint64        now);

/**
 * clawt_fed_peer_selector_json:
 * @peer: a #ClawtFedPeer
 * @health: its link's state
 *
 * What clawt_fed_selector_matches() and a client read.
 *
 * Returns: (transfer full): `{alias, label, fleet_id, tier, owner, tags,
 *   health, hidden, muted, status, endpoint}`
 */
JsonObject *clawt_fed_peer_selector_json(const ClawtFedPeer *peer,
                                         ClawtFedHealth      health);

/**
 * clawt_fed_peer_display:
 * @peer: a #ClawtFedPeer
 *
 * Returns: (transfer none): the label, or the alias when it has none
 */
const gchar *clawt_fed_peer_display(const ClawtFedPeer *peer);

/**
 * clawt_fed_alias_check:
 * @alias: (nullable): a proposed alias
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE if @alias is a valid id of at most %CLAWT_FED_ALIAS_MAX
 */
gboolean clawt_fed_alias_check(const gchar  *alias,
                               GError      **error);

/**
 * clawt_fed_alias_suggest:
 * @name: (nullable): a fleet's display name
 *
 * Returns: (transfer full): an alias made from @name (lowercased, other
 *   characters folded to `-`), or `peer` when nothing is left
 */
gchar *clawt_fed_alias_suggest(const gchar *name);

#define CLAWT_TYPE_FED_PENDING_INVITE (clawt_fed_pending_invite_get_type())

/**
 * ClawtFedPendingInvite:
 * @id: the invite's id
 * @secret: its secret
 * @tier: the tier the accepting fleet will get here
 * @expires: unix seconds
 * @confirm: whether an accept waits for the operator's approval
 * @note: (nullable): who it was for, as the operator wrote it
 * @created_at: unix seconds
 *
 * An invite this fleet has issued and nobody has used yet.
 */
typedef struct {
    gchar        *id;
    gchar        *secret;
    ClawtFedTier  tier;
    gint64        expires;
    gboolean      confirm;
    gchar        *note;
    gint64        created_at;
} ClawtFedPendingInvite;

GType clawt_fed_pending_invite_get_type(void) G_GNUC_CONST;

/**
 * clawt_fed_pending_invite_copy:
 * @invite: a #ClawtFedPendingInvite
 *
 * Returns: (transfer full): a deep copy
 */
ClawtFedPendingInvite *clawt_fed_pending_invite_copy(const ClawtFedPendingInvite *invite);

/**
 * clawt_fed_pending_invite_free:
 * @invite: (transfer full) (nullable): a #ClawtFedPendingInvite
 */
void clawt_fed_pending_invite_free(ClawtFedPendingInvite *invite);

#define CLAWT_TYPE_FED_REGISTRY (clawt_fed_registry_get_type())
G_DECLARE_FINAL_TYPE(ClawtFedRegistry, clawt_fed_registry, CLAWT, FED_REGISTRY, GObject)

/**
 * clawt_fed_registry_new:
 * @peers_path: `<state_dir>/federation/peers.yaml`
 * @invites_path: `<secrets.dir>/federation/invites.yaml`
 *
 * Returns: (transfer full): an empty registry; call
 *   clawt_fed_registry_load()
 */
ClawtFedRegistry *clawt_fed_registry_new(const gchar *peers_path,
                                         const gchar *invites_path);

/**
 * clawt_fed_registry_load:
 * @self: a #ClawtFedRegistry
 * @error: (out) (optional): return location for a #GError
 *
 * Missing files are an empty registry.  A peer entry that cannot be read
 * is skipped with a warning naming it, never dropped from the file: the
 * next save writes back what was read, so it is set aside in
 * `peers.yaml.unreadable` first.
 *
 * Returns: %TRUE unless a file exists and is not YAML
 */
gboolean clawt_fed_registry_load(ClawtFedRegistry  *self,
                                 GError           **error);

/**
 * clawt_fed_registry_save:
 * @self: a #ClawtFedRegistry
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE when both files were written
 */
gboolean clawt_fed_registry_save(ClawtFedRegistry  *self,
                                 GError           **error);

/**
 * clawt_fed_registry_list:
 * @self: a #ClawtFedRegistry
 *
 * Returns: (transfer container) (element-type ClawtFedPeer): every peer,
 *   by alias; the peers belong to the registry
 */
GPtrArray *clawt_fed_registry_list(ClawtFedRegistry *self);

/**
 * clawt_fed_registry_get:
 * @self: a #ClawtFedRegistry
 * @alias: an alias
 *
 * Returns: (transfer none) (nullable): the peer
 */
ClawtFedPeer *clawt_fed_registry_get(ClawtFedRegistry *self,
                                     const gchar      *alias);

/**
 * clawt_fed_registry_by_fleet:
 * @self: a #ClawtFedRegistry
 * @fleet_id: a fingerprint
 *
 * Returns: (transfer none) (nullable): the peer with that fleet id
 */
ClawtFedPeer *clawt_fed_registry_by_fleet(ClawtFedRegistry *self,
                                          const gchar      *fleet_id);

/**
 * clawt_fed_registry_add:
 * @self: a #ClawtFedRegistry
 * @peer: (transfer full): the peer
 * @error: (out) (optional): return location for a #GError
 *
 * Refuses an invalid alias, an alias in use and a fleet already linked
 * (under any alias) -- a second record for one fleet would make every
 * grant ambiguous.
 *
 * Returns: %TRUE if added
 */
gboolean clawt_fed_registry_add(ClawtFedRegistry  *self,
                                ClawtFedPeer      *peer,
                                GError           **error);

/**
 * clawt_fed_registry_remove:
 * @self: a #ClawtFedRegistry
 * @alias: an alias
 *
 * Returns: %TRUE if there was such a peer
 */
gboolean clawt_fed_registry_remove(ClawtFedRegistry *self,
                                   const gchar      *alias);

/**
 * clawt_fed_registry_add_invite:
 * @self: a #ClawtFedRegistry
 * @invite: (transfer full): the invite
 */
void clawt_fed_registry_add_invite(ClawtFedRegistry      *self,
                                   ClawtFedPendingInvite *invite);

/**
 * clawt_fed_registry_find_invite:
 * @self: a #ClawtFedRegistry
 * @id: an invite id
 *
 * Returns: (transfer none) (nullable): the invite
 */
ClawtFedPendingInvite *clawt_fed_registry_find_invite(ClawtFedRegistry *self,
                                                      const gchar      *id);

/**
 * clawt_fed_registry_remove_invite:
 * @self: a #ClawtFedRegistry
 * @id: an invite id
 *
 * Returns: %TRUE if there was such an invite
 */
gboolean clawt_fed_registry_remove_invite(ClawtFedRegistry *self,
                                          const gchar      *id);

/**
 * clawt_fed_registry_list_invites:
 * @self: a #ClawtFedRegistry
 *
 * Returns: (transfer container) (element-type ClawtFedPendingInvite): the
 *   outstanding invites
 */
GPtrArray *clawt_fed_registry_list_invites(ClawtFedRegistry *self);

/**
 * clawt_fed_registry_prune_invites:
 * @self: a #ClawtFedRegistry
 * @now: unix seconds
 *
 * Returns: how many expired invites were dropped
 */
guint clawt_fed_registry_prune_invites(ClawtFedRegistry *self,
                                       gint64            now);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtFedPeer, clawt_fed_peer_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtFedPendingInvite, clawt_fed_pending_invite_free)

G_END_DECLS
