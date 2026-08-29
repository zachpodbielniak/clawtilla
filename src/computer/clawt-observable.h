/*
 * clawt-observable.h - A computer that can show you what is on its screen
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * An interface rather than another vfunc on #ClawtComputer, because most
 * computers have no screen at all and a vfunc every backend has to
 * answer is a vfunc four of them answer badly.  A container with no
 * desktop should say so; it should not return a blank frame that reads
 * as a black screen.
 *
 * So: a backend that can produce a picture implements this, one that
 * cannot does not, and a client asks clawt_computer_type_has_screen()
 * rather than keeping a list of its own -- the rule
 * clawt_computer_type_has_machine() already sets, for the same reason.
 * A type added later reaches every surface without any of them being
 * edited.
 *
 * Nothing here is called while a client waits.  Every implementation
 * talks to a compositor over a socket or an SSH connection, so the
 * grabbing lives on #ClawtObserver's worker thread and this interface is
 * free to block.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-enums.h"

G_BEGIN_DECLS

/**
 * ClawtInputEvent:
 * @kind: which sort of event this is
 * @text: (nullable): the key name or combo for %CLAWT_INPUT_KEY, or the
 *   string to type for %CLAWT_INPUT_TEXT
 * @x: the pointer's x, in the screen's own pixels
 * @y: the pointer's y
 * @button: 1 left, 2 middle, 3 right
 * @dx: horizontal scroll
 * @dy: vertical scroll
 *
 * One thing a person or an agent wants done to a screen.
 *
 * A boxed record rather than five methods, because every backend has to
 * marshal the same five shapes and a per-shape vfunc means five places
 * for one of them to be forgotten -- which is invisible until somebody
 * scrolls.
 *
 * Keys travel as a **name**, not a keyval: gowl takes an XKB name and
 * gnome-desktop-mcp's KeyCombo takes a string, so a number here would be
 * converted twice and wrongly at least once.
 */
typedef struct {
    ClawtInputKind  kind;
    gchar          *text;
    gint            x;
    gint            y;
    guint           button;
    gdouble         dx;
    gdouble         dy;
} ClawtInputEvent;

#define CLAWT_TYPE_INPUT_EVENT (clawt_input_event_get_type())

GType clawt_input_event_get_type(void) G_GNUC_CONST;

/**
 * clawt_input_event_new:
 * @kind: which sort of event
 *
 * Returns: (transfer full): a zeroed event of that kind
 */
ClawtInputEvent *clawt_input_event_new(ClawtInputKind kind);

ClawtInputEvent *clawt_input_event_copy(ClawtInputEvent *self);
void             clawt_input_event_free(ClawtInputEvent *self);

/**
 * clawt_input_event_set_text:
 * @self: a #ClawtInputEvent
 * @text: (nullable): the key name, combo, or string to type
 */
void clawt_input_event_set_text(ClawtInputEvent *self, const gchar *text);

/**
 * clawt_input_event_describe:
 * @self: a #ClawtInputEvent
 *
 * A one-line description for a log or an audit trail.
 *
 * The typed text is **not** included: a takeover is somebody driving
 * their own desktop, and what they type into it is not clawtilla's to
 * write down.
 *
 * Returns: (transfer full): the description
 */
gchar *clawt_input_event_describe(ClawtInputEvent *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtInputEvent, clawt_input_event_free)

/* ── The interface ───────────────────────────────────────────────── */

#define CLAWT_TYPE_OBSERVABLE (clawt_observable_get_type())

G_DECLARE_INTERFACE(ClawtObservable, clawt_observable, CLAWT, OBSERVABLE,
                    GObject)

/**
 * ClawtObservableInterface:
 * @observe_start: make the screen ready to be grabbed at @fps
 * @observe_stop: stop, releasing whatever start took
 * @observe_frame: one picture, or %NULL
 * @observe_can_input: whether this screen accepts injected events
 * @observe_send_input: inject one
 * @observe_viewer_uri: a URI a real viewer can open, or %NULL
 *
 * What a backend with a screen implements.
 *
 * Every vfunc is optional and every missing one **refuses**, naming the
 * type -- never answers %TRUE.  A screen that reports a frame sent and
 * sends none is worse than one that says it cannot: the person watching
 * has been told the picture in front of them is current.
 */
struct _ClawtObservableInterface {
    GTypeInterface parent_iface;

    gboolean  (*observe_start)     (ClawtObservable  *self,
                                    guint             fps,
                                    GError          **error);
    void      (*observe_stop)      (ClawtObservable  *self);

    GBytes *  (*observe_frame)     (ClawtObservable  *self,
                                    const gchar      *if_changed_from,
                                    gint64           *stamp_out,
                                    gchar           **hash_out,
                                    GError          **error);

    gboolean  (*observe_can_input) (ClawtObservable  *self);
    gboolean  (*observe_send_input)(ClawtObservable  *self,
                                    ClawtInputEvent  *event,
                                    GError          **error);

    gchar *   (*observe_viewer_uri)(ClawtObservable  *self);

    gboolean  (*observe_geometry)  (ClawtObservable  *self,
                                    guint            *width,
                                    guint            *height);
};

/**
 * clawt_observable_start:
 * @self: a #ClawtObservable
 * @fps: frames a second somebody is asking for
 * @error: (out) (optional): return location for a #GError
 *
 * Tells the backend somebody has started watching.
 *
 * @fps is clamped by clawt_observe_clamp_fps() before it arrives, so a
 * backend never has to defend against a zero or a hundred.
 *
 * Returns: %TRUE if the screen can be grabbed
 */
gboolean clawt_observable_start(ClawtObservable  *self,
                                guint             fps,
                                GError          **error);

/**
 * clawt_observable_stop:
 * @self: a #ClawtObservable
 *
 * Tells the backend the last watcher has gone.
 */
void clawt_observable_stop(ClawtObservable *self);

/**
 * clawt_observable_frame:
 * @self: a #ClawtObservable
 * @if_changed_from: (nullable): a hash from an earlier frame
 * @stamp_out: (out) (optional): when the picture was taken, in
 *   microseconds since the epoch
 * @hash_out: (out) (optional) (transfer full): a content hash of the
 *   frame, whether or not any bytes came back
 * @error: (out) (optional): return location for a #GError
 *
 * One picture of the screen.
 *
 * Three outcomes, and telling them apart is the whole contract:
 *
 * - bytes, with @hash_out set: a new frame;
 * - %NULL with no error and @hash_out equal to @if_changed_from: the
 *   screen has not moved, so nothing was read or transferred;
 * - %NULL with @error set: it could not be grabbed, and the message is
 *   the backend's own rather than a summary of it.
 *
 * The unchanged case is what makes polling affordable.  An idle desktop
 * is the ordinary state of a machine being supervised, and reading and
 * shipping an identical PNG once a second for an hour is a cost paid for
 * nothing.
 *
 * Returns: (transfer full) (nullable): the frame as PNG bytes
 */
GBytes *clawt_observable_frame(ClawtObservable  *self,
                               const gchar      *if_changed_from,
                               gint64           *stamp_out,
                               gchar           **hash_out,
                               GError          **error);

/**
 * clawt_observable_can_input:
 * @self: a #ClawtObservable
 *
 * Whether this screen takes injected keys and pointer events at all.
 *
 * A separate question from whether the *agent* may send them, which is
 * `computer.desktop.allow_input` and is enforced where the agent's tool
 * calls pass.  This one is about the backend.
 *
 * Returns: %TRUE if input can be sent
 */
gboolean clawt_observable_can_input(ClawtObservable *self);

/**
 * clawt_observable_send_input:
 * @self: a #ClawtObservable
 * @event: what to do
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE if it was sent
 */
gboolean clawt_observable_send_input(ClawtObservable  *self,
                                     ClawtInputEvent  *event,
                                     GError          **error);

/**
 * clawt_observable_viewer_uri:
 * @self: a #ClawtObservable
 *
 * A URI a real remote-desktop viewer can open, when there is one.
 *
 * The polled frames are a supervision view: cheap, slow and safe to
 * leave open.  A VM also has a VNC server on it, and somebody who wants
 * to actually use that desktop wants the viewer rather than a picture a
 * second -- so the two are offered side by side rather than one being
 * made to pretend to be the other.
 *
 * %NULL when there is nothing to open, which for a VM includes the
 * ordinary case of a domain that is not running: naming a port on a dead
 * guest sends somebody to debug their viewer.
 *
 * Returns: (transfer full) (nullable): the URI
 */
gchar *clawt_observable_viewer_uri(ClawtObservable *self);

/**
 * clawt_observable_geometry:
 * @self: a #ClawtObservable
 * @width: (out) (optional): the screen's width in its own pixels
 * @height: (out) (optional): its height
 *
 * How big the screen actually is.
 *
 * Needed because the frame is **not** that size: it is downscaled in the
 * compositor, so a click at a point in the picture is not a click at
 * that point on the screen. Both halves of the ratio have to be known
 * somewhere, and a client scaling by a number it guessed would miss by a
 * factor of three on a HiDPI guest -- which reads as the pointer being
 * unreliable rather than as arithmetic.
 *
 * Unknown answers %FALSE and leaves the outputs at zero, which the
 * clients render as "coordinates are in the picture's own pixels"
 * rather than silently mis-clicking. It may block, so it is called from
 * #ClawtObserver's worker thread and its answer cached.
 *
 * Returns: %TRUE if the size is known
 */
gboolean clawt_observable_geometry(ClawtObservable *self,
                                   guint           *width,
                                   guint           *height);

/**
 * CLAWT_OBSERVE_MAX_FPS:
 *
 * The ceiling on `computer.desktop.observe_fps`.
 *
 * Grabbing shares the one channel the agent is using for its own work --
 * an SSH connection into a guest, or a compositor's single-threaded
 * socket -- so every frame is latency taken from the task somebody is
 * watching.  Four a second is already generous for a supervision view
 * and it is not a video call.
 */
#define CLAWT_OBSERVE_MAX_FPS 4

/**
 * CLAWT_OBSERVE_MIN_GAP_MS:
 *
 * The shortest gap between two grabs, whatever asked for them.
 *
 * The interval timer and an explicit refresh share one capture path, and
 * this is what stops somebody holding the refresh button turning a
 * supervision view into a denial of service against the agent's own
 * connection.
 */
#define CLAWT_OBSERVE_MIN_GAP_MS 250

/**
 * CLAWT_OBSERVE_LEASE_SECONDS:
 *
 * How long a watcher counts for without asking again.
 *
 * A browser tab cannot say goodbye -- closing it sends nothing -- so a
 * subscription that only ended when a client asked would leave a screen
 * being grabbed for ever after somebody shut their laptop, and the only
 * symptom would be the agent being slower. Every poll pushes the lease
 * forward, so a client that stops asking stops being watched.
 *
 * Comfortably longer than the slowest poll a client makes, because a
 * lease shorter than the interval would drop a watcher between two of
 * its own requests and restart the watch on the next -- which shows up
 * as a preview that flickers back to "no frame yet".
 */
#define CLAWT_OBSERVE_LEASE_SECONDS 30

/**
 * clawt_observe_clamp_fps:
 * @fps: what the configuration or a client asked for
 *
 * Brings a requested rate inside 1..%CLAWT_OBSERVE_MAX_FPS.
 *
 * One function rather than a clamp at each caller: three of them would
 * be three answers to what zero means, and zero -- which is what an
 * unset integer key reads as -- has to mean the default rather than
 * "never grab", or a client subscribing would sit in front of a blank
 * panel with nothing to say why.
 *
 * Returns: a usable rate
 */
guint clawt_observe_clamp_fps(gint64 fps);

/**
 * CLAWT_FRAME_STALE_SECONDS:
 *
 * How old a frame may be before it stops counting as current.
 *
 * Five seconds is five missed grabs at the default rate, which no
 * healthy watch produces -- so it means the connection has gone rather
 * than that the screen is quiet.
 */
#define CLAWT_FRAME_STALE_SECONDS 5

/**
 * clawt_frame_is_stale:
 * @stamp_us: when the frame was taken, microseconds since the epoch
 * @now_us: the time to compare against
 *
 * Whether a frame should be labelled with its age rather than presented
 * as what is on the screen now.
 *
 * In the library because both clients need the same answer, and a
 * threshold spelled out in each of them would be two thresholds that
 * agreed until somebody tuned one. The failure it exists to prevent is
 * the quiet one: a picture from ten minutes ago, drawn without comment,
 * is somebody supervising a machine that stopped answering and not
 * knowing it.
 *
 * A frame that has never been taken (@stamp_us of 0) is **not** stale --
 * there is nothing to label, and a client showing "0 seconds ago" over
 * an empty panel would be describing a picture that does not exist.
 *
 * Returns: %TRUE if the frame needs its age shown
 */
gboolean clawt_frame_is_stale(gint64 stamp_us, gint64 now_us);

G_END_DECLS
