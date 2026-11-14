/*
 * clawt-device-store.h - Paired client devices and their tokens
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * One bearer token for every client meant that a lost laptop or phone was
 * a rotation for every other client too, on a daemon that binds its
 * tailnet address by default.  A device is paired instead: the owner asks
 * for a short-lived six-digit code, the new device trades it for a token
 * of its own, and that token can be revoked -- or left to expire after a
 * stretch of idleness -- without touching anything else.
 *
 * The daemon's own token (`daemon.token_file`, or `<state_dir>/tcp-token`)
 * stays the owner credential.  Nothing here reads or replaces it.
 *
 * Tokens are stored hashed (SHA-256) in `<state_dir>/devices.yaml`, mode
 * 0600, so the file is not itself a set of credentials, and every
 * comparison is constant-time.  A token is returned exactly once, from
 * clawt_device_store_redeem(), to the device being paired.  Nothing that
 * lists devices can see one: #ClawtDevice does not carry it.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-types.h"

G_BEGIN_DECLS

/**
 * CLAWT_DEVICE_CODE_DIGITS:
 *
 * How long a pairing code is.  Six digits is what a person can read off
 * one screen and type into another without a second look.
 */
#define CLAWT_DEVICE_CODE_DIGITS (6)

/**
 * CLAWT_DEVICE_CODE_LIFETIME_SECONDS:
 *
 * How long a pairing code is good for.  Long enough to walk to the other
 * device, short enough that a code read over a shoulder is useless by
 * the time anybody tries it.
 */
#define CLAWT_DEVICE_CODE_LIFETIME_SECONDS (300)

/**
 * CLAWT_DEVICE_CODE_MAX_GUESSES:
 *
 * Wrong guesses a pairing code survives.  The next one discards it, so
 * guessing a code has at most this many chances in a million rather than
 * as many as the network can carry in five minutes.
 */
#define CLAWT_DEVICE_CODE_MAX_GUESSES (5)

/**
 * CLAWT_DEVICE_REDEEM_FAILURES_PER_MINUTE:
 *
 * Failed redemptions, of any code, the daemon accepts in a minute before
 * it refuses to look at another.  The guess limit protects one code; this
 * protects the next one somebody asks for while a guesser is still busy.
 */
#define CLAWT_DEVICE_REDEEM_FAILURES_PER_MINUTE (10)

#define CLAWT_TYPE_DEVICE (clawt_device_get_type())

/**
 * ClawtDevice:
 *
 * One paired device, as a listing sees it: an id, a name and two times.
 * There is no token here and no hash -- a listing cannot leak what it
 * was never given.
 */
typedef struct _ClawtDevice ClawtDevice;

GType clawt_device_get_type(void) G_GNUC_CONST;

/**
 * clawt_device_copy:
 * @self: a #ClawtDevice
 *
 * Returns: (transfer full): a copy of @self
 */
ClawtDevice *clawt_device_copy(ClawtDevice *self);

/**
 * clawt_device_free:
 * @self: (transfer full) (nullable): a #ClawtDevice
 *
 * Releases @self.
 */
void clawt_device_free(ClawtDevice *self);

/**
 * clawt_device_get_id:
 * @self: a #ClawtDevice
 *
 * Returns: (transfer none): the id `device revoke` takes
 */
const gchar *clawt_device_get_id(ClawtDevice *self);

/**
 * clawt_device_get_name:
 * @self: a #ClawtDevice
 *
 * Returns: (transfer none): what the device is called
 */
const gchar *clawt_device_get_name(ClawtDevice *self);

/**
 * clawt_device_get_created:
 * @self: a #ClawtDevice
 *
 * Returns: when it was paired, in seconds since the epoch
 */
gint64 clawt_device_get_created(ClawtDevice *self);

/**
 * clawt_device_get_last_seen:
 * @self: a #ClawtDevice
 *
 * Returns: when it last authenticated or was last connected, in seconds
 *   since the epoch
 */
gint64 clawt_device_get_last_seen(ClawtDevice *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtDevice, clawt_device_free)

#define CLAWT_TYPE_DEVICE_STORE (clawt_device_store_get_type())

G_DECLARE_FINAL_TYPE(ClawtDeviceStore, clawt_device_store, CLAWT,
                     DEVICE_STORE, GObject)

/**
 * clawt_device_store_new:
 * @path: where the hashed tokens live (normally `<state_dir>/devices.yaml`)
 *
 * Creates an empty store.  Call clawt_device_store_load() to read @path.
 *
 * Returns: (transfer full): a new #ClawtDeviceStore
 */
ClawtDeviceStore *clawt_device_store_new(const gchar *path);

/**
 * clawt_device_store_load:
 * @self: a #ClawtDeviceStore
 * @error: (out) (optional): return location for a #GError
 *
 * Reads the store's file.  A file that does not exist is an empty store.
 *
 * Returns: %TRUE on success
 */
gboolean clawt_device_store_load(ClawtDeviceStore  *self,
                                 GError           **error);

/**
 * clawt_device_store_get_path:
 * @self: a #ClawtDeviceStore
 *
 * Returns: (transfer none): the file this store reads and writes
 */
const gchar *clawt_device_store_get_path(ClawtDeviceStore *self);

/**
 * clawt_device_store_set_idle_seconds:
 * @self: a #ClawtDeviceStore
 * @seconds: how long a device may go unseen before its token stops
 *   working; 0 for never
 *
 * The idle window, from `daemon.device_idle_days`.  Applied when a token
 * is presented as well as by clawt_device_store_expire_idle(), so a
 * device that comes back after the window is refused at once rather than
 * at the next sweep.
 */
void clawt_device_store_set_idle_seconds(ClawtDeviceStore *self,
                                         gint64            seconds);

/**
 * clawt_device_store_begin_pairing:
 * @self: a #ClawtDeviceStore
 * @name: (nullable): what to call the device that redeems it
 * @now: the current time, in seconds since the epoch
 * @expires_at: (out) (optional): when the code stops working
 * @error: (out) (optional): return location for a #GError
 *
 * Issues a fresh pairing code, replacing any code still outstanding.
 *
 * The digits come from `/dev/urandom` and are drawn without modulo bias.
 * One code at a time on purpose: two outstanding codes would be twice
 * the chance of a guess landing, and nobody pairs two devices at once.
 *
 * Returns: (transfer full) (nullable): the code, #CLAWT_DEVICE_CODE_DIGITS
 *   digits, or %NULL on error
 */
gchar *clawt_device_store_begin_pairing(ClawtDeviceStore  *self,
                                        const gchar       *name,
                                        gint64             now,
                                        gint64            *expires_at,
                                        GError           **error);

/**
 * clawt_device_store_pairing_expires_at:
 * @self: a #ClawtDeviceStore
 * @now: the current time, in seconds since the epoch
 *
 * Whether a code is outstanding, without saying what it is.
 *
 * Returns: when the outstanding code expires, or 0 when there is none
 */
gint64 clawt_device_store_pairing_expires_at(ClawtDeviceStore *self,
                                             gint64            now);

/**
 * clawt_device_store_cancel_pairing:
 * @self: a #ClawtDeviceStore
 *
 * Discards the outstanding code, if there is one.
 */
void clawt_device_store_cancel_pairing(ClawtDeviceStore *self);

/**
 * clawt_device_store_redeem:
 * @self: a #ClawtDeviceStore
 * @code: what the device was given
 * @name: (nullable): what the device calls itself; used when the code
 *   was issued without a name
 * @now: the current time, in seconds since the epoch
 * @device_id: (out) (optional) (transfer full): the new device's id
 * @error: (out) (optional): return location for a #GError
 *
 * Trades a pairing code for a token of the device's own.
 *
 * The code is compared in constant time and is single use.  A wrong
 * guess counts against it, and after #CLAWT_DEVICE_CODE_MAX_GUESSES the
 * code is discarded; failures beyond
 * #CLAWT_DEVICE_REDEEM_FAILURES_PER_MINUTE in a minute are refused
 * without looking at the code at all.  Refusals are %CLAWT_ERROR_AUTH,
 * or %CLAWT_ERROR_LOOP_LIMIT for the rate limit.
 *
 * Returns: (transfer full) (nullable): the token -- the only time it is
 *   ever produced -- or %NULL on error
 */
gchar *clawt_device_store_redeem(ClawtDeviceStore  *self,
                                 const gchar       *code,
                                 const gchar       *name,
                                 gint64             now,
                                 gchar            **device_id,
                                 GError           **error);

/**
 * clawt_device_store_authenticate:
 * @self: a #ClawtDeviceStore
 * @token: (nullable): what a client presented
 * @now: the current time, in seconds since the epoch
 *
 * Which device, if any, @token belongs to.
 *
 * Every stored hash is compared, in constant time, whether or not an
 * earlier one matched.  A device idle past the window is removed and
 * refused.  A match updates the device's last-seen time.
 *
 * Returns: (transfer full) (nullable): the device's id, or %NULL
 */
gchar *clawt_device_store_authenticate(ClawtDeviceStore *self,
                                       const gchar      *token,
                                       gint64            now);

/**
 * clawt_device_store_touch:
 * @self: a #ClawtDeviceStore
 * @id: a device id
 * @now: the current time, in seconds since the epoch
 *
 * Records that @id is still connected.  A client that stays connected
 * for a month never presents its token again, and without this it would
 * expire while somebody was looking at it.  Written to disk only every
 * few minutes, not on every call.
 */
void clawt_device_store_touch(ClawtDeviceStore *self,
                              const gchar      *id,
                              gint64            now);

/**
 * clawt_device_store_list:
 * @self: a #ClawtDeviceStore
 *
 * Returns: (transfer full) (element-type ClawtDevice): every paired
 *   device, oldest first
 */
GPtrArray *clawt_device_store_list(ClawtDeviceStore *self);

/**
 * clawt_device_store_lookup:
 * @self: a #ClawtDeviceStore
 * @id: a device id
 *
 * Returns: (transfer full) (nullable): a copy of that device, or %NULL
 */
ClawtDevice *clawt_device_store_lookup(ClawtDeviceStore *self,
                                       const gchar      *id);

/**
 * clawt_device_store_revoke:
 * @self: a #ClawtDeviceStore
 * @id: a device id
 * @error: (out) (optional): return location for a #GError
 *
 * Forgets @id, so its token stops working.  The caller closes whatever
 * connections it already had -- the store knows tokens, not sockets.
 *
 * Returns: %TRUE when it was there and is gone
 */
gboolean clawt_device_store_revoke(ClawtDeviceStore  *self,
                                   const gchar       *id,
                                   GError           **error);

/**
 * clawt_device_store_expire_idle:
 * @self: a #ClawtDeviceStore
 * @now: the current time, in seconds since the epoch
 *
 * Removes every device idle past the window.
 *
 * Returns: (transfer full) (element-type utf8): the ids removed, possibly
 *   none
 */
GPtrArray *clawt_device_store_expire_idle(ClawtDeviceStore *self,
                                          gint64            now);

/**
 * clawt_device_token_hash:
 * @token: a device token
 *
 * The form a token is stored in: SHA-256, lower-case hex.
 *
 * Returns: (transfer full): the hash
 */
gchar *clawt_device_token_hash(const gchar *token);

/**
 * clawt_device_code_normalize:
 * @text: (nullable): what somebody typed
 *
 * A pairing code as typed, with the spaces and dashes people put in to
 * read it taken out again -- "123 456" and "123-456" are the code the
 * CLI printed.
 *
 * Returns: (transfer full) (nullable): the digits, or %NULL when @text is
 *   not #CLAWT_DEVICE_CODE_DIGITS digits once they are gone
 */
gchar *clawt_device_code_normalize(const gchar *text);

/**
 * clawt_device_code_display:
 * @code: a pairing code
 *
 * The code split in two, "123 456", which is how every client shows it.
 *
 * Returns: (transfer full): the text to show
 */
gchar *clawt_device_code_display(const gchar *code);

/**
 * clawt_device_countdown_text:
 * @remaining: seconds left
 *
 * "4:59", or "expired" -- what every client draws beside a code.
 *
 * Returns: (transfer full): the text to show
 */
gchar *clawt_device_countdown_text(gint64 remaining);

/**
 * clawt_device_seen_text:
 * @last_seen: when the device was last seen, seconds since the epoch
 * @connected: whether it has a connection open right now
 * @now: the current time, in seconds since the epoch
 *
 * "connected now", "seen 3 days ago" -- one phrasing for the CLI and
 * every client, since a device list is read to decide which device is
 * the one that went missing.
 *
 * Returns: (transfer full): the text to show
 */
gchar *clawt_device_seen_text(gint64   last_seen,
                              gboolean connected,
                              gint64   now);

G_END_DECLS
