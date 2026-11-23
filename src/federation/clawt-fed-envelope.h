/*
 * clawt-fed-envelope.h - What one fleet says to another, signed
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Every transport carries the same thing: a body (JSON text) and a
 * stamp saying who sent it, to whom, when, and a signature over all of
 * that.  Security lives here and not in the pipe, which is what lets a
 * peer reach us through a Cloudflare tunnel, a tailnet, or a LAN with no
 * TLS and get the same guarantees from each:
 *
 *  - authenticity: signed by the sender's fleet key;
 *  - integrity: the body digest is signed;
 *  - no replay: a timestamp inside a window and a nonce never seen twice;
 *  - no redirection: the receiving fleet is signed too, so a request
 *    meant for one fleet cannot be replayed at another;
 *  - answers are signed as well, bound to the request's nonce -- a TLS
 *    terminator (Cloudflare) proves nothing about who answered.
 *
 * The signed text is
 *
 *     clawtilla-fed/1 \n kind \n from \n to \n time \n nonce \n
 *     sha256(body) \n binding
 *
 * where binding is `METHOD PATH` on HTTP, `re:<nonce>` for an answer, and
 * empty otherwise.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>
#include "federation/clawt-fleet-key.h"

G_BEGIN_DECLS

/**
 * CLAWT_FED_PROTOCOL:
 *
 * The first line of every signed text, and the protocol a hello names.
 */
#define CLAWT_FED_PROTOCOL "clawtilla-fed/1"

/**
 * CLAWT_FED_DEFAULT_SKEW_SECONDS:
 *
 * How far a stamp's time may be from ours, either way, by default.
 */
#define CLAWT_FED_DEFAULT_SKEW_SECONDS (300)

/**
 * CLAWT_FED_NONCES_PER_PEER:
 *
 * How many nonces the replay guard keeps per peer.  A peer sending more
 * than this inside one window is over any rate a grant allows; the oldest
 * is forgotten, and its time is already close to the window's edge.
 */
#define CLAWT_FED_NONCES_PER_PEER (4096)

#define CLAWT_TYPE_FED_STAMP (clawt_fed_stamp_get_type())

/**
 * ClawtFedStamp:
 * @kind: what the body is (`hello`, `message`, `task.create`, `reply`, ...)
 * @from: the sending fleet's id
 * @to: the receiving fleet's id; empty only on an invite accept, when the
 *   sender has not met the receiver yet
 * @time: unix seconds when it was signed
 * @nonce: random, never repeated by the sender
 * @digest: lowercase hex SHA-256 of the body
 * @binding: what else was signed (see the file comment)
 * @key_index: which of the sender's keys signed it (rotation)
 * @signature: base64url Ed25519 signature
 *
 * The signed header of an envelope.  The body travels beside it.
 */
typedef struct {
    gchar  *kind;
    gchar  *from;
    gchar  *to;
    gint64  time;
    gchar  *nonce;
    gchar  *digest;
    gchar  *binding;
    guint   key_index;
    gchar  *signature;
} ClawtFedStamp;

GType clawt_fed_stamp_get_type(void) G_GNUC_CONST;

/**
 * clawt_fed_stamp_copy:
 * @stamp: a #ClawtFedStamp
 *
 * Returns: (transfer full): a deep copy
 */
ClawtFedStamp *clawt_fed_stamp_copy(const ClawtFedStamp *stamp);

/**
 * clawt_fed_stamp_free:
 * @stamp: (transfer full) (nullable): a #ClawtFedStamp
 */
void clawt_fed_stamp_free(ClawtFedStamp *stamp);

/**
 * clawt_fed_digest:
 * @body: (array length=length) (nullable): the body
 * @length: its length
 *
 * Returns: (transfer full): lowercase hex SHA-256
 */
gchar *clawt_fed_digest(const gchar *body,
                        gsize        length);

/**
 * clawt_fed_signed_text:
 * @stamp: a stamp (its @signature is ignored)
 *
 * Returns: (transfer full): exactly the text a signature covers
 */
gchar *clawt_fed_signed_text(const ClawtFedStamp *stamp);

/**
 * clawt_fed_stamp_sign:
 * @key: our fleet key
 * @key_index: the index peers know @key by
 * @kind: what the body is
 * @from: our fleet id
 * @to: (nullable): theirs
 * @body: (nullable): the body, JSON text
 * @binding: (nullable): `METHOD PATH`, `re:<nonce>`, or %NULL
 * @error: (out) (optional): return location for a #GError
 *
 * Stamps @body with a fresh nonce and the current time.
 *
 * Returns: (transfer full) (nullable): the stamp, or %NULL when the
 *   kernel pool could not supply a nonce
 */
ClawtFedStamp *clawt_fed_stamp_sign(ClawtFleetKey *key,
                                    guint          key_index,
                                    const gchar   *kind,
                                    const gchar   *from,
                                    const gchar   *to,
                                    const gchar   *body,
                                    const gchar   *binding,
                                    GError       **error);

/**
 * clawt_fed_stamp_check:
 * @stamp: a received stamp
 * @public_key: (array fixed-size=32): the sender's key at
 *   @stamp->key_index, looked up by the caller from @stamp->from
 * @expect_to: (nullable): our fleet id; %NULL skips the check (invite
 *   accept only)
 * @body: (nullable): the body that arrived with it
 * @binding: (nullable): the binding we expect (`POST /fed/v1/envelope`);
 *   the signature is checked against this, whatever the stamp claims
 * @now: unix seconds
 * @skew: the window, seconds, either way
 * @error: (out) (optional): return location for a #GError
 *
 * Everything but the nonce: digest, binding, recipient, time and the
 * signature.  The nonce is clawt_fed_replay_guard_admit()'s, because it
 * needs state.  Refusals are %CLAWT_ERROR_AUTH and deliberately say
 * little -- the detail is for our log, not for whoever sent it.
 *
 * Returns: %TRUE if the stamp is good
 */
gboolean clawt_fed_stamp_check(const ClawtFedStamp *stamp,
                               const guint8        *public_key,
                               const gchar         *expect_to,
                               const gchar         *body,
                               const gchar         *binding,
                               gint64               now,
                               guint                skew,
                               GError             **error);

/**
 * clawt_fed_stamp_to_json:
 * @stamp: a #ClawtFedStamp
 *
 * Returns: (transfer full): `{kind, from, to, time, nonce, digest,
 *   binding, key, sig}`
 */
JsonObject *clawt_fed_stamp_to_json(const ClawtFedStamp *stamp);

/**
 * clawt_fed_stamp_from_json:
 * @object: (nullable): what clawt_fed_stamp_to_json() made
 * @error: (out) (optional): return location for a #GError
 *
 * Refuses a stamp with a member missing, of the wrong type, or longer
 * than any real one would be -- before anything is verified.
 *
 * Returns: (transfer full) (nullable): the stamp
 */
ClawtFedStamp *clawt_fed_stamp_from_json(JsonObject  *object,
                                         GError     **error);

/**
 * ClawtFedHeaderFunc:
 * @name: a header name (`X-Clawt-Fed-From`, ...)
 * @value: its value
 * @user_data: closure
 *
 * Receives one header of a stamp.
 */
typedef void (*ClawtFedHeaderFunc)(const gchar *name,
                                   const gchar *value,
                                   gpointer     user_data);

/**
 * ClawtFedHeaderLookup:
 * @name: a header name
 * @user_data: closure
 *
 * Returns: (nullable): the header's value
 */
typedef const gchar *(*ClawtFedHeaderLookup)(const gchar *name,
                                             gpointer     user_data);

/**
 * clawt_fed_stamp_to_headers:
 * @stamp: a #ClawtFedStamp
 * @func: (scope call): called once per header
 * @user_data: closure
 *
 * The HTTP spelling of a stamp, one `X-Clawt-Fed-*` header per member.
 * Toolkit-free: the caller sets them on whatever message it has.
 */
void clawt_fed_stamp_to_headers(const ClawtFedStamp *stamp,
                                ClawtFedHeaderFunc   func,
                                gpointer             user_data);

/**
 * clawt_fed_stamp_from_headers:
 * @lookup: (scope call): reads one header
 * @user_data: closure
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the stamp the headers carry
 */
ClawtFedStamp *clawt_fed_stamp_from_headers(ClawtFedHeaderLookup   lookup,
                                            gpointer               user_data,
                                            GError               **error);

/**
 * clawt_fed_envelope_build:
 * @stamp: the stamp
 * @body: (nullable): the body, JSON text
 *
 * The one-object form the WebSocket and the IPC transport carry:
 * `{"stamp": {...}, "body": "<json text>"}`.  The body stays a string so
 * the bytes that were signed are the bytes that arrive.
 *
 * Returns: (transfer full): the envelope object
 */
JsonObject *clawt_fed_envelope_build(const ClawtFedStamp *stamp,
                                     const gchar         *body);

/**
 * clawt_fed_envelope_split:
 * @envelope: (nullable): what clawt_fed_envelope_build() made
 * @out_body: (out) (transfer full): the body text
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the stamp
 */
ClawtFedStamp *clawt_fed_envelope_split(JsonObject  *envelope,
                                        gchar      **out_body,
                                        GError     **error);

#define CLAWT_TYPE_FED_REPLAY_GUARD (clawt_fed_replay_guard_get_type())

/**
 * ClawtFedReplayGuard:
 *
 * Remembers the nonces each peer has used inside the time window, so a
 * captured request cannot be sent twice.  Bounded per peer.
 */
typedef struct _ClawtFedReplayGuard ClawtFedReplayGuard;

GType clawt_fed_replay_guard_get_type(void) G_GNUC_CONST;

/**
 * clawt_fed_replay_guard_new:
 * @skew: the window, seconds either way
 *
 * Returns: (transfer full): an empty guard
 */
ClawtFedReplayGuard *clawt_fed_replay_guard_new(guint skew);

/**
 * clawt_fed_replay_guard_ref:
 * @guard: a #ClawtFedReplayGuard
 *
 * Returns: (transfer full): @guard
 */
ClawtFedReplayGuard *clawt_fed_replay_guard_ref(ClawtFedReplayGuard *guard);

/**
 * clawt_fed_replay_guard_unref:
 * @guard: (transfer full): a #ClawtFedReplayGuard
 */
void clawt_fed_replay_guard_unref(ClawtFedReplayGuard *guard);

/**
 * clawt_fed_replay_guard_admit:
 * @guard: a #ClawtFedReplayGuard
 * @peer: the sending fleet's id
 * @nonce: the stamp's nonce
 * @time: the stamp's time
 * @now: unix seconds
 *
 * Call only after clawt_fed_stamp_check() succeeded, so an unsigned
 * request cannot fill the table.
 *
 * Returns: %TRUE the first time a nonce is seen inside the window
 */
gboolean clawt_fed_replay_guard_admit(ClawtFedReplayGuard *guard,
                                      const gchar         *peer,
                                      const gchar         *nonce,
                                      gint64               time,
                                      gint64               now);

/**
 * clawt_fed_replay_guard_save:
 * @guard: a #ClawtFedReplayGuard
 * @path: where to write
 * @now: unix seconds; entries already outside the window are dropped
 * @error: (out) (optional): return location for a #GError
 *
 * Writes what the guard remembers (0600), so a restart inside the window
 * cannot re-admit a request it already took.
 *
 * Returns: %TRUE on success
 */
gboolean clawt_fed_replay_guard_save(ClawtFedReplayGuard *guard,
                                     const gchar         *path,
                                     gint64               now,
                                     GError             **error);

/**
 * clawt_fed_replay_guard_load:
 * @guard: a #ClawtFedReplayGuard
 * @path: what clawt_fed_replay_guard_save() wrote
 * @now: unix seconds
 *
 * Reads a saved table.  A missing or damaged file loads nothing: the
 * window is minutes long, and a damaged table must not stop a daemon
 * starting.
 */
void clawt_fed_replay_guard_load(ClawtFedReplayGuard *guard,
                                 const gchar         *path,
                                 gint64               now);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtFedStamp, clawt_fed_stamp_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtFedReplayGuard, clawt_fed_replay_guard_unref)

G_END_DECLS
