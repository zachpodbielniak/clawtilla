/*
 * clawt-guest-demo-recorder.h - Somebody showing the fleet how, in a VM
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The same keylogger as the host recorder, in the agent's own guest,
 * over gnome-desktop-mcp's `StartRecording`/`DrainRecording`/
 * `StopRecording`.
 *
 * The guard here is better than gowl's and still not complete, and both
 * halves of that need saying.  GNOME Shell can see that the actor
 * holding key focus is a password entry, so capture pauses for its lock
 * screen, its polkit prompt and its keyring prompt -- and the trace
 * carries a marker for the gap rather than simply missing eight seconds.
 * But an application window inside that session is a Wayland client like
 * any other, and what is inside it is private to it: a password typed
 * into a browser's login form is recorded.
 *
 * Recording has its **own consent flag** in the extension, which
 * `SetEnabled` does not touch and no D-Bus method can set.  A refusal is
 * therefore ordinary rather than a fault, and its text -- which names
 * the switch and the `gsettings` line -- is passed through rather than
 * summarised.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-types.h"
#include "computer/clawt-computer.h"
#include "teach/clawt-teach-recorder.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_GUEST_DEMO_RECORDER \
    (clawt_guest_demo_recorder_get_type())

G_DECLARE_FINAL_TYPE(ClawtGuestDemoRecorder, clawt_guest_demo_recorder,
                     CLAWT, GUEST_DEMO_RECORDER, ClawtTeachRecorder)

/**
 * clawt_guest_demo_recorder_new:
 * @id: the recording's id
 * @directory: where the trace goes
 * @agent_id: whose VM this is
 * @computer: (transfer none): that agent's computer, which must be a VM
 *
 * Returns: (transfer full) (nullable): the recorder, or %NULL when @id
 *   is not a usable recording id or @computer is not a VM
 */
ClawtGuestDemoRecorder *clawt_guest_demo_recorder_new(
    const gchar   *id,
    const gchar   *directory,
    const gchar   *agent_id,
    ClawtComputer *computer);

/**
 * clawt_guest_demo_recorder_absorb:
 * @self: a #ClawtGuestDemoRecorder
 * @events_json: the JSON array `DrainRecording` answered with
 * @dropped: how many the ring lost since the last drain
 * @error: (out) (optional): return location for a #GError
 *
 * Turns the extension's events into steps and counters.
 *
 * Public and separate from the SSH round trip that fetches them, so
 * that the part with the decisions in it can be tested against a string
 * literal.  The transport needs a booted guest and cannot be.
 *
 * Motion is not kept as a step, for the reason the host recorder gives:
 * the extension samples it every 20 ms, and a trace of a drag would be
 * a thousand steps of the pointer travelling with the click that
 * mattered pushed out of the budget.  `suppressed` and `resumed` markers
 * **are** kept -- a gap a trace does not explain reads as somebody
 * having done nothing.
 *
 * Returns: %TRUE if @events_json was understood
 */
gboolean clawt_guest_demo_recorder_absorb(ClawtGuestDemoRecorder  *self,
                                          const gchar             *events_json,
                                          guint                    dropped,
                                          GError                 **error);

G_END_DECLS
