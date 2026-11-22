/*
 * clawt-office-director.h - Who stands where: presence and a world into a layout
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The presence fold says what each participant is doing; the world says
 * what places exist; the choreography says which place each activity
 * goes to.  The director puts them together into a *layout*: for every
 * participant, the zone, the seat in it, the pose, the prop, the emote,
 * and who it is looking at.  Renderers only walk people to where the
 * layout says and draw what it says they hold.
 *
 * It lives in the library, and the daemon serves its answer
 * (`office.layout`), because three renderers -- the libregnum office,
 * the GTK embed and the web page -- deciding seats on their own would
 * put the same agent in three chairs.
 *
 * Deterministic: the same presence and world give the same seats,
 * whichever order participants were added in, so a refresh never
 * shuffles the room.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <json-glib/json-glib.h>

#include "office/clawt-office-world.h"

G_BEGIN_DECLS

/**
 * clawt_office_direct:
 * @world: the world
 * @rules: the choreography
 * @snapshot: an `office.snapshot` object (`participants`)
 *
 * Places every participant.  Each element of the result's `placements`
 * carries `id`, `zone`, `zone_tag`, `seat` (index), `x`/`y` (2D map
 * pixels) and `x3`/`z3` (3D level units) when the zone has such anchors,
 * `pose`, `prop`, `emote`, `facing` (a participant id or %NULL), and
 * `fallback` with the reason when the zone the choreography asked for
 * does not exist in @world (the lounge is used instead).
 *
 * Seats are taken in a fixed order: meetings first (they need several
 * together), then everybody else by id; within a zone the first free
 * seat, the team's own desks first.  A full zone overflows to the next
 * zone with the same tag, then stands at the zone's last anchor.
 *
 * Returns: (transfer full): `{world, placements: [...]}`
 */
JsonNode *clawt_office_direct(ClawtOfficeWorld        *world,
                              ClawtOfficeChoreography *rules,
                              JsonObject              *snapshot);

G_END_DECLS
