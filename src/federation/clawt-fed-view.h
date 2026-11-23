/*
 * clawt-fed-view.h - How a linked fleet is worded, in every client
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The GTK, web and libregnum clients each draw the Federation page from
 * the same `federation.peer.list` reply.  The line under a peer's name,
 * an audit row and the health dot's character are made here, once, so
 * the three say the same thing.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * clawt_fed_peer_summary:
 * @peer: (nullable): one entry of `federation.peer.list`
 *
 * Returns: (transfer full): e.g. "Trusted · Alice · #lab #gpu · live ·
 *   12 ms", or "Waiting for your approval" for a pending one
 */
gchar *clawt_fed_peer_summary(JsonObject *peer);

/**
 * clawt_fed_peer_title:
 * @peer: (nullable): one entry of `federation.peer.list`
 *
 * Returns: (transfer full): the label, and the alias when it differs
 */
gchar *clawt_fed_peer_title(JsonObject *peer);

/**
 * clawt_fed_health_glyph:
 * @health: (nullable): a health nick
 *
 * Returns: (transfer none): ● live, ◐ degraded or connecting, ○ offline,
 *   ✕ revoked, ◌ anything else
 */
const gchar *clawt_fed_health_glyph(const gchar *health);

/**
 * clawt_fed_audit_line:
 * @entry: (nullable): one entry of `federation.audit`
 * @now: unix seconds
 *
 * Returns: (transfer full): e.g. "12:04 in lab hello ok"
 */
gchar *clawt_fed_audit_line(JsonObject *entry,
                            gint64      now);

G_END_DECLS
