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

/**
 * ClawtFedChip:
 * @kind: `tier`, `owner`, `tag` or `online`
 * @label: what the chip says
 * @term: the selector term it stands for (`tier:trusted`, `tag:gpu`,
 *   `owner:Alice`, `online`)
 *
 * One filter chip above the linked fleets in a sidebar.
 */
typedef struct {
    gchar *kind;
    gchar *label;
    gchar *term;
} ClawtFedChip;

/**
 * clawt_fed_chip_free:
 * @chip: (transfer full) (nullable): a #ClawtFedChip
 */
void clawt_fed_chip_free(ClawtFedChip *chip);

/**
 * clawt_fed_filter_chips:
 * @fleets: (nullable): `agent.list`'s `fleets`
 *
 * The chips worth offering for these fleets, in one order every client
 * draws: `online`, then each tier present (most trusted first, walked
 * from the enum), each owner, each tag -- never a chip that would match
 * nothing.
 *
 * Returns: (transfer full) (element-type ClawtFedChip): the chips
 */
GPtrArray *clawt_fed_filter_chips(JsonArray *fleets);

/**
 * clawt_fed_filter_matches:
 * @fleet: one entry of `fleets`
 * @terms: (array zero-terminated=1) (nullable): the chips switched on
 * @show_hidden: whether hidden fleets are drawn
 *
 * Chips of one kind are alternatives, kinds combine: `tier:trusted`,
 * `tier:owned` and `tag:gpu` is a trusted-or-owned fleet tagged gpu.  No
 * chips matches every fleet that is not hidden.
 *
 * Returns: %TRUE if the fleet is drawn
 */
gboolean clawt_fed_filter_matches(JsonObject  *fleet,
                                  gchar      **terms,
                                  gboolean     show_hidden);

/**
 * clawt_fed_agent_presence:
 * @agent: one remote agent from a fleet's `agents`
 * @out_tone: (out) (optional) (transfer none): `success`, `accent` or
 *   `neutral`
 *
 * Returns: (transfer none): `working`, `idle`, `stopped` or `unknown`
 */
const gchar *clawt_fed_agent_presence(JsonObject   *agent,
                                      const gchar **out_tone);

/**
 * clawt_fed_badge_tone:
 * @tone: (nullable): a tone the federation code gives (`success`,
 *   `accent`, `warning`, `error`, `neutral`)
 *
 * The same tone in the badge vocabulary the web and lrg clients draw
 * with, so neither keeps its own translation of the library's words.
 * The GTK client uses @tone as it is: those are libadwaita's classes.
 *
 * Returns: (transfer none): `good`, `info`, `warn`, `bad` or `neutral`
 */
const gchar *clawt_fed_badge_tone(const gchar *tone);

G_END_DECLS
