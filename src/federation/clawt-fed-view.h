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

/**
 * clawt_fed_ref_parse:
 * @ref: `AGENT@ALIAS`, how a person writes another fleet's agent
 * @out_agent: (out) (optional) (transfer full): the agent id
 * @out_alias: (out) (optional) (transfer full): the linked fleet's alias
 *
 * Both halves must be ids as clawtilla spells one (clawt_is_valid_id()),
 * so `zach@bob.com` is not a ref.  `user@ALIAS` is that fleet's operator.
 *
 * Returns: %TRUE if @ref is one
 */
gboolean clawt_fed_ref_parse(const gchar  *ref,
                             gchar       **out_agent,
                             gchar       **out_alias);

/**
 * clawt_fed_sender_id:
 * @alias: the linked fleet, as known here
 * @agent: its agent (or `user`)
 *
 * Returns: (transfer full): `fleet:ALIAS:AGENT`, the sender id a remote
 *   agent's messages carry here -- a #CLAWT_SENDER_REMOTE
 */
gchar *clawt_fed_sender_id(const gchar *alias,
                           const gchar *agent);

/**
 * clawt_fed_sender_parse:
 * @sender: a sender id
 * @out_alias: (out) (optional) (transfer full): the fleet's alias
 * @out_agent: (out) (optional) (transfer full): its agent
 *
 * Returns: %TRUE if @sender is `fleet:ALIAS:AGENT`
 */
gboolean clawt_fed_sender_parse(const gchar  *sender,
                                gchar       **out_alias,
                                gchar       **out_agent);

/**
 * clawt_fed_room_id:
 * @alias: the linked fleet
 * @local: our side: an agent id or `user`
 * @remote: their agent (or `user`)
 *
 * The room a conversation with another fleet's agent happens in.  A colon
 * marks it derived (clawt_room_is_declared()), so it can never collide
 * with a room somebody named.
 *
 * Returns: (transfer full): `fed:ALIAS:LOCAL:REMOTE`
 */
gchar *clawt_fed_room_id(const gchar *alias,
                         const gchar *local,
                         const gchar *remote);

/**
 * clawt_fed_room_parse:
 * @room_id: a room id
 * @out_alias: (out) (optional) (transfer full): the linked fleet
 * @out_local: (out) (optional) (transfer full): our side
 * @out_remote: (out) (optional) (transfer full): their agent
 *
 * Returns: %TRUE if @room_id is a `fed:` room with three valid ids
 */
gboolean clawt_fed_room_parse(const gchar  *room_id,
                              gchar       **out_alias,
                              gchar       **out_local,
                              gchar       **out_remote);

/**
 * clawt_fed_fence:
 * @who: who wrote @text, for the label (`atlas@lab`)
 * @text: another fleet's words
 *
 * @text with any forged `[clawtilla]` marker escaped, inside a backtick
 * fence longer than any backtick run in it -- so nothing it says can
 * close the fence and speak as clawtilla.
 *
 * Returns: (transfer full): the fenced text
 */
gchar *clawt_fed_fence(const gchar *who,
                       const gchar *text);

/**
 * clawt_fed_room_title:
 * @room_id: a room id
 *
 * What a conversation with another fleet's agent is called: `atlas@lab`
 * for the operator's, `support ↔ atlas@lab` for an agent's.  The room
 * manager names the room with it, and a client titles the chat with it.
 *
 * A room another fleet shares (`fedroom:`) is `ROOM @ HOST`.
 *
 * Returns: (transfer full) (nullable): the title, or %NULL when @room_id
 *   is not a `fed:` or `fedroom:` room
 */
gchar *clawt_fed_room_title(const gchar *room_id);

/**
 * clawt_fed_room_banner:
 * @room_id: a room id
 * @fleets: (nullable): `agent.list`'s `fleets`
 *
 * The line a chat with another fleet's agent carries under its title, so
 * nobody forgets whom they are writing to: the fleet, its tier, its owner
 * and whether it is reachable now.
 *
 * Returns: (transfer full) (nullable): the line, or %NULL when @room_id
 *   is not a `fed:` room
 */
gchar *clawt_fed_room_banner(const gchar *room_id,
                             JsonArray   *fleets);

/**
 * clawt_fed_shared_room_id:
 * @host: the alias of the fleet that hosts the room
 * @room: its id there
 *
 * The local mirror of a room another fleet shares with this one (0.70.0).
 *
 * Returns: (transfer full): `fedroom:HOST:ROOM`
 */
gchar *clawt_fed_shared_room_id(const gchar *host,
                                const gchar *room);

/**
 * clawt_fed_shared_room_parse:
 * @room_id: a room id
 * @out_host: (out) (optional) (transfer full): the hosting fleet's alias
 * @out_room: (out) (optional) (transfer full): the room's id there
 *
 * Returns: %TRUE if @room_id is a `fedroom:` mirror
 */
gboolean clawt_fed_shared_room_parse(const gchar  *room_id,
                                     gchar       **out_host,
                                     gchar       **out_room);

/**
 * clawt_fed_sparkline:
 * @values: (nullable): numbers, oldest first
 *
 * The numbers as block characters, one each, scaled to their largest.
 *
 * Returns: (transfer full): the line, empty for no values
 */
gchar *clawt_fed_sparkline(JsonArray *values);

/**
 * clawt_fed_weather_line:
 * @fleet: a peer listing or an `agent.list` fleet, with `weather`
 *
 * A link's weather in a line (0.71.0): its traffic over the last hour in
 * five-minute steps, and the latency of its last answers.  Every client
 * draws this, so they agree on what the bars mean.
 *
 * Returns: (transfer full): the line, empty when there is no weather
 */
gchar *clawt_fed_weather_line(JsonObject *fleet);

/**
 * clawt_fed_traffic_spark:
 * @fleet: as for clawt_fed_weather_line()
 *
 * The last twenty minutes' traffic, for a heading that shares its line
 * with the fleet's name; empty when the link was quiet.
 *
 * Returns: (transfer full): the sparkline, or ""
 */
gchar *clawt_fed_traffic_spark(JsonObject *fleet);

/**
 * clawt_fed_post_text:
 * @room: the shared room's id at its host
 * @id: the post's id where it was written
 * @from: who wrote it, as their own fleet names them (`user`, `atlas`)
 * @body: what they wrote
 *
 * What the author fleet signs for a word in a shared room (0.72.0), so a
 * third fleet can check who wrote it however many hosts relayed it.
 *
 * Returns: (transfer full): the canonical text
 */
gchar *clawt_fed_post_text(const gchar *room,
                           const gchar *id,
                           const gchar *from,
                           const gchar *body);

G_END_DECLS
