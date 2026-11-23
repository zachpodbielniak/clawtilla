/*
 * clawt-fed-store.h - federation.db: what federation keeps between restarts
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * One SQLite file, <state_dir>/federation/federation.db (0.73.0), for
 * everything federation must not lose: the audit, the nonces the replay
 * guard has seen, the outbox of envelopes waiting for a fleet, what became
 * of each message sent (a conversation's delivery ticks), and a small keyed
 * table for the rest (held messages, task costs, shared rooms).  Peers stay
 * in peers.yaml, which a person may read; secrets stay in the secrets
 * directory.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define CLAWT_TYPE_FED_STORE (clawt_fed_store_get_type())
G_DECLARE_FINAL_TYPE(ClawtFedStore, clawt_fed_store, CLAWT, FED_STORE, GObject)

/**
 * clawt_fed_store_open:
 * @path: the database file, made (0600) if it is not there
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the store, or %NULL
 */
ClawtFedStore *clawt_fed_store_open(const gchar  *path,
                                    GError      **error);

/**
 * clawt_fed_store_audit_add:
 * @self: a #ClawtFedStore
 * @entry: `{time, direction, peer, kind, outcome, detail}`
 */
void clawt_fed_store_audit_add(ClawtFedStore *self,
                               JsonObject    *entry);

/**
 * clawt_fed_store_audit_list:
 * @self: a #ClawtFedStore
 * @peer: (nullable): only this alias
 * @limit: at most this many
 *
 * Returns: (transfer full): the entries, newest first
 */
JsonArray *clawt_fed_store_audit_list(ClawtFedStore *self,
                                      const gchar   *peer,
                                      guint          limit);

/**
 * clawt_fed_store_audit_prune:
 * @self: a #ClawtFedStore
 * @before: unix seconds; older entries go
 */
void clawt_fed_store_audit_prune(ClawtFedStore *self,
                                 gint64         before);

/**
 * clawt_fed_store_nonce_add:
 * @self: a #ClawtFedStore
 * @fleet: whose
 * @nonce: the nonce it used
 * @time: its stamp's time
 */
void clawt_fed_store_nonce_add(ClawtFedStore *self,
                               const gchar   *fleet,
                               const gchar   *nonce,
                               gint64         time);

/**
 * clawt_fed_store_nonces:
 * @self: a #ClawtFedStore
 * @since: unix seconds; older ones are dropped, not returned
 *
 * Returns: (transfer full): `[{fleet, nonce, time}]`, oldest first, for
 *   the replay guard to take back after a restart
 */
JsonArray *clawt_fed_store_nonces(ClawtFedStore *self,
                                  gint64         since);

/**
 * clawt_fed_store_outbox_put:
 * @self: a #ClawtFedStore
 * @out: `{id, alias, kind, body, room?, queued}`
 */
void clawt_fed_store_outbox_put(ClawtFedStore *self,
                                JsonObject    *out);

/**
 * clawt_fed_store_outbox_remove:
 * @self: a #ClawtFedStore
 * @id: the envelope's id
 */
void clawt_fed_store_outbox_remove(ClawtFedStore *self,
                                   const gchar   *id);

/**
 * clawt_fed_store_outbox:
 * @self: a #ClawtFedStore
 *
 * Returns: (transfer full): every waiting envelope, oldest first
 */
JsonArray *clawt_fed_store_outbox(ClawtFedStore *self);

/**
 * clawt_fed_store_delivery_set:
 * @self: a #ClawtFedStore
 * @message: the message's id
 * @room: the conversation it was said in
 * @state: queued, sent, delivered, held, declined, refused or expired
 * @detail: (nullable): why, for anything but delivered
 */
void clawt_fed_store_delivery_set(ClawtFedStore *self,
                                  const gchar   *message,
                                  const gchar   *room,
                                  const gchar   *state,
                                  const gchar   *detail);

/**
 * clawt_fed_store_deliveries:
 * @self: a #ClawtFedStore
 * @room: a conversation
 *
 * Returns: (transfer full): message id -> `{state, detail?}`, for drawing
 *   a conversation's ticks
 */
JsonObject *clawt_fed_store_deliveries(ClawtFedStore *self,
                                       const gchar   *room);

/**
 * clawt_fed_store_kv_set:
 * @self: a #ClawtFedStore
 * @table: which kind of thing (`holds`, `costs`, `shares`)
 * @key: its key
 * @value: (nullable): the value, or %NULL to remove it
 */
void clawt_fed_store_kv_set(ClawtFedStore *self,
                            const gchar   *table,
                            const gchar   *key,
                            JsonObject    *value);

/**
 * clawt_fed_store_kv_all:
 * @self: a #ClawtFedStore
 * @table: which kind of thing
 *
 * Returns: (transfer full): key -> value
 */
JsonObject *clawt_fed_store_kv_all(ClawtFedStore *self,
                                   const gchar   *table);

G_END_DECLS
