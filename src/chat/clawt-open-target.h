/*
 * clawt-open-target.h - What a `clawtilla:` link or `--open` names
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#ifndef CLAWT_OPEN_TARGET_H
#define CLAWT_OPEN_TARGET_H

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * CLAWT_OPEN_TARGET_MAX:
 *
 * The longest target a link may name, in bytes.  A direct room's id is
 * two agent ids (64 each) and a prefix, so this leaves room for every
 * id the daemon spells and refuses a pasted paragraph.
 */
#define CLAWT_OPEN_TARGET_MAX (200)

/**
 * clawt_open_target_normalize:
 * @text: what was passed to `--open`, or the path of a link
 * @error: return location for a #GError, or %NULL
 *
 * The room id @text names, with the link's own spelling taken off:
 * `clawtilla:chief`, `clawtilla://chief`, `clawtilla://chief/` and
 * `chief` all name `chief`.  Surrounding whitespace is dropped.
 *
 * Refused, with %CLAWT_ERROR_INVALID_ARGUMENT and a message naming what
 * is allowed: an empty target, one longer than %CLAWT_OPEN_TARGET_MAX,
 * and one holding anything but lower-case letters, digits, `-`, `_`,
 * `.`, `:` and `@` -- the characters agent and room ids are made of, and
 * the `@` of another fleet's agent (`atlas@lab`, 0.72.0), which a client
 * opens through `federation.open` rather than clawt_open_target_resolve().  The
 * check is on the spelling only; whether the room exists is
 * clawt_open_target_resolve()'s question, and needs the daemon.
 *
 * Returns: (transfer full) (nullable): the room id, or %NULL on error
 */
gchar *
clawt_open_target_normalize(
	const gchar  *text,
	GError      **error
);

/**
 * clawt_open_target_resolve:
 * @target: a room id from clawt_open_target_normalize()
 * @agents: (nullable): the `agents` array of an `agent.list` reply
 * @rooms: (nullable): the `rooms` array of a `room.list` reply
 * @out_agent: (out) (optional) (transfer full) (nullable): the agent
 *   whose chat to open
 * @out_peer: (out) (optional) (transfer full) (nullable): the peer
 *   conversation of @out_agent to open, when the target is one
 * @out_room: (out) (optional) (transfer full) (nullable): the group
 *   room to open
 * @error: return location for a #GError, or %NULL
 *
 * Which conversation @target is, checked against what exists.
 *
 * - A room a client lists (clawt_room_is_listed()) opens that room;
 *   it is looked for first, so a declared room keeps its link even
 *   when an agent of the same name exists.
 * - An agent's id, or its `dm_room`, opens that agent's chat.
 * - A direct room between two agents opens the first member's
 *   conversation with the second, the way the conversation switcher
 *   does.
 *
 * On success exactly one of @out_agent and @out_room is set.  Anything
 * else -- no such agent or room, or a room no client opens (a routine's
 * room) -- is %CLAWT_ERROR_NOT_FOUND with a sentence that names the
 * target, which every client shows as it is.
 *
 * Returns: %TRUE when @target names something a client can open
 */
gboolean
clawt_open_target_resolve(
	const gchar  *target,
	JsonArray    *agents,
	JsonArray    *rooms,
	gchar       **out_agent,
	gchar       **out_peer,
	gchar       **out_room,
	GError      **error
);

/**
 * clawt_open_target_from_argv:
 * @argv: (array zero-terminated=1): another invocation's whole command
 *   line, program name first
 *
 * The `--open` (or `-o`, or `--open=`) in a command line a single-instance
 * client was handed by a second invocation, or %NULL when it names none.
 * Every other option is ignored -- the invocation that typed them already
 * checked them -- and the last `--open` wins, as it would have there.
 * The value is returned as written: normalising it is the window's job,
 * so a link that does not parse is reported where somebody is looking.
 *
 * Returns: (transfer full) (nullable): the target, or %NULL
 */
gchar *
clawt_open_target_from_argv(
	const gchar * const *argv
);

/**
 * clawt_open_target_hold:
 * @pending: (inout) (transfer full) (nullable): where a target waiting
 *   for the daemon is kept
 * @target: (transfer full): a target to open
 * @connected: whether the client is connected and can open it now
 *
 * Whether to open @target now or keep it for later.  Not connected, it
 * replaces whatever was waiting in @pending (clicking a second link
 * before the daemon answers means the second) and %NULL is returned;
 * opening it then would refuse a daemon that is merely starting and lose
 * the link.  Connected, it is returned to open now and @pending is left
 * alone -- the window opens what waited when the connection arrives.
 *
 * Returns: (transfer full) (nullable): the target to open now, or %NULL
 *   when it is held
 */
gchar *
clawt_open_target_hold(
	gchar    **pending,
	gchar     *target,
	gboolean   connected
);

G_END_DECLS

#endif /* CLAWT_OPEN_TARGET_H */
