/*
 * clawt-gowl-demo-recorder.h - Somebody showing the fleet how, on gowl
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * This is a keylogger.  Not as a figure of speech: while it runs, every
 * key pressed on the machine's real desktop is written into a file
 * clawtilla keeps, and then read back in a chat window by whoever
 * reviews the draft.
 *
 * gowl's own guard is honest about where it stops, and that honesty has
 * to survive the trip through this layer.  Under Wayland a client's
 * widget tree is private to the client, so gowl **cannot** recognise a
 * password field the way GNOME Shell can.  It suppresses capture while
 * the session is locked and while the focused window's app-id or title
 * matches a deny list of credential applications, and that is the whole
 * of it -- a password typed into a form inside an ordinary window the
 * deny list does not name is recorded.  gowl puts that sentence in every
 * payload it produces; clawtilla puts it on the trace, in the draft, in
 * both clients and in `docs/teach.org`, because the place somebody
 * decides a trace is safe to share is not the place a document is.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-types.h"
#include "teach/clawt-teach-recorder.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_GOWL_DEMO_RECORDER (clawt_gowl_demo_recorder_get_type())

G_DECLARE_FINAL_TYPE(ClawtGowlDemoRecorder, clawt_gowl_demo_recorder,
                     CLAWT, GOWL_DEMO_RECORDER, ClawtTeachRecorder)

/**
 * clawt_gowl_demo_recorder_new:
 * @id: the recording's id
 * @directory: where the trace goes
 * @socket_path: gowl's MCP socket, from `computer.desktop.socket`
 *
 * Returns: (transfer full) (nullable): the recorder, or %NULL when @id
 *   is not a usable recording id
 */
ClawtGowlDemoRecorder *clawt_gowl_demo_recorder_new(
    const gchar *id,
    const gchar *directory,
    const gchar *socket_path);

/**
 * clawt_gowl_demo_recorder_absorb:
 * @self: a #ClawtGowlDemoRecorder
 * @payload: what `drain_recording` or `stop_recording` answered
 * @error: (out) (optional): return location for a #GError
 *
 * Turns one of gowl's payloads into steps, counters and caveats.
 *
 * Separate from the socket call and public on purpose: the transport
 * needs a compositor and cannot be tested, and this -- which is where
 * every decision actually is -- needs only a string.  What the tests
 * drive is this.
 *
 * Pointer **motion is deliberately not kept as a step.** A single drag
 * is hundreds of motions even after gowl coalesces them, and a trace
 * made mostly of them would spend its whole event budget on the mouse
 * travelling and evict the clicks and keystrokes the demonstration is
 * about. Every button and scroll carries the position it happened at,
 * so nothing a procedure needs is lost -- and the trace says so rather
 * than leaving a reader to wonder where the pointer went.
 *
 * Returns: %TRUE if @payload was understood
 */
gboolean clawt_gowl_demo_recorder_absorb(ClawtGowlDemoRecorder  *self,
                                         const gchar            *payload,
                                         GError                **error);

G_END_DECLS
