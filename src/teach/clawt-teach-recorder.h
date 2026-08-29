/*
 * clawt-teach-recorder.h - Watching a task being done, so it can be reused
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Three things can be watched -- an agent working, a person on the
 * host's desktop, a person inside an agent's VM -- and they differ only
 * in where the steps come from.  Everything else is the same and lives
 * here: the bounds, the deadline that ends a forgotten recording, the
 * frames, the caveats, and writing the trace out.  Three copies of that
 * would be three answers to "what happens when the limit is reached",
 * and only one of them would have been tested.
 *
 * A derivable type rather than a switch, because a plugin should be able
 * to add a fourth.  Every vfunc that is missing **refuses, naming the
 * type**: a recorder that reported a demonstration started and captured
 * nothing is worse than one that says it cannot, because the person
 * demonstrating finds out at the end.
 *
 * Nothing here is safe to call while a client waits.  Two of the three
 * recorders talk to a compositor -- one over a unix socket, one over
 * SSH into a guest -- so start, stop and drain each have an async form
 * that takes the wait onto a worker thread, and the IPC handlers use
 * those.  The rule this tree keeps having to re-apply is that the wait
 * belongs at the function rather than at whichever call site somebody
 * noticed it from.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <gio/gio.h>
#include <glib-object.h>

#include "clawt-types.h"
#include "computer/clawt-observer.h"
#include "teach/clawt-teach-trace.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_TEACH_RECORDER (clawt_teach_recorder_get_type())

G_DECLARE_DERIVABLE_TYPE(ClawtTeachRecorder, clawt_teach_recorder,
                         CLAWT, TEACH_RECORDER, GObject)

/**
 * ClawtTeachRecorderClass:
 * @start: begin capturing; the base has already made the directory and
 *   stamped the trace by the time this is called
 * @stop: stop capturing and take whatever is left
 * @drain: take what has been captured so far, leaving it running
 *
 * What a recorder has to supply.
 *
 * All three refuse by default, naming the type.  A subclass with
 * genuinely nothing to do for one of them still overrides it and says so
 * -- #ClawtAgentTraceRecorder's `drain` is a real implementation that
 * returns %TRUE because its steps are pushed in rather than pulled, and
 * that is a different statement from the base's refusal.
 */
struct _ClawtTeachRecorderClass {
    GObjectClass parent_class;

    gboolean (*start)(ClawtTeachRecorder *self, GError **error);
    gboolean (*stop) (ClawtTeachRecorder *self, GError **error);
    gboolean (*drain)(ClawtTeachRecorder *self, GError **error);

    gpointer padding[8];
};

/**
 * clawt_teach_recorder_get_id:
 * @self: a #ClawtTeachRecorder
 *
 * Returns: (transfer none): the recording's id
 */
const gchar *clawt_teach_recorder_get_id(ClawtTeachRecorder *self);

/**
 * clawt_teach_recorder_get_trace:
 * @self: a #ClawtTeachRecorder
 *
 * Returns: (transfer none): the trace, complete as far as it has got
 */
ClawtTeachTrace *clawt_teach_recorder_get_trace(ClawtTeachRecorder *self);

/**
 * clawt_teach_recorder_is_active:
 * @self: a #ClawtTeachRecorder
 *
 * Returns: %TRUE while it is capturing
 */
gboolean clawt_teach_recorder_is_active(ClawtTeachRecorder *self);

/**
 * clawt_teach_recorder_set_limits:
 * @self: a #ClawtTeachRecorder
 * @max_seconds: `skills.teach_max_seconds`, or 0 for the default
 * @max_events: `skills.teach_max_events`, or 0 for the default
 *
 * Both bounds, before the recording starts.
 *
 * Zero means the default rather than "no limit", for the reason the
 * observer's fps clamp gives: an unset integer key reads as zero, and a
 * recording with no deadline is precisely the failure the deadline
 * exists to prevent.
 */
void clawt_teach_recorder_set_limits(ClawtTeachRecorder *self,
                                     gint64              max_seconds,
                                     gint64              max_events);

guint clawt_teach_recorder_get_max_seconds(ClawtTeachRecorder *self);
guint clawt_teach_recorder_get_max_events(ClawtTeachRecorder *self);

/**
 * clawt_teach_recorder_set_observer:
 * @self: a #ClawtTeachRecorder
 * @observer: (nullable) (transfer none): the daemon's #ClawtObserver
 * @agent_id: (nullable): whose screen
 * @computer: (nullable) (transfer none): that agent's computer
 *
 * Attaches a picture to each step, by joining the watch that already
 * exists.
 *
 * The recorder subscribes as an ordinary watcher for the length of the
 * recording, so frames are grabbed on #ClawtObserver's worker thread at
 * the configured rate and shared with anybody else looking -- rather
 * than this opening a second capture path with its own timing, its own
 * rate and its own bugs.  A frame costs the agent either way; two would
 * cost it twice.
 *
 * A computer with no screen is not an error here.  The recording runs
 * with steps and no frames, and the trace says so -- which is the whole
 * of what `computer.type: none` means for teaching.
 *
 * **Only for a recorder whose steps arrive on the daemon's own thread.**
 * The observer is not thread-safe, and the two demonstration recorders
 * produce their steps on a worker thread, so they do not set one. The
 * base checks rather than trusting: a frame is only ever attached from
 * the thread the recorder was built on.
 *
 * @fps is what the agent's `computer.desktop.observe_fps` says, clamped
 * by the observer.
 */
void clawt_teach_recorder_set_observer(ClawtTeachRecorder *self,
                                       ClawtObserver      *observer,
                                       const gchar        *agent_id,
                                       ClawtComputer      *computer,
                                       gint64              fps);

/**
 * clawt_teach_recorder_join_watch:
 * @self: a #ClawtTeachRecorder
 * @fps: frames a second to ask for
 *
 * Joins the agent's screen watch, as an ordinary subscriber.
 *
 * Called by the base from the first step rather than from start, and
 * exposed because a subclass may want the watch running earlier.  Must
 * be called from the thread the recorder was built on.
 *
 * A refusal is not a failure: the reason is recorded on the trace as a
 * caveat and the observer is released, so a screen that cannot be
 * watched costs one refused call rather than one per step.
 *
 * Returns: %TRUE if the watch was joined
 */
gboolean clawt_teach_recorder_join_watch(ClawtTeachRecorder *self,
                                         gint64              fps);

/**
 * clawt_teach_recorder_adopt_trace:
 * @self: a #ClawtTeachRecorder
 * @trace: (transfer none): the trace this recorder fills
 *
 * For a subclass's constructor: the trace is the recorder's identity.
 *
 * Set once, before anything is recorded.  The watcher name the observer
 * knows this recording by is derived from the trace's id here, so that
 * two recordings of one agent's screen are two watchers rather than one
 * that the second unsubscribe stops for both.
 */
void clawt_teach_recorder_adopt_trace(ClawtTeachRecorder *self,
                                      ClawtTeachTrace    *trace);

/**
 * clawt_teach_recorder_start:
 * @self: a #ClawtTeachRecorder
 * @error: (out) (optional): return location for a #GError
 *
 * Makes the trace directory, stamps the start, arms the deadline, and
 * asks the subclass to begin.
 *
 * The deadline's timer is attached to the context that is thread-default
 * **here**, captured in the function that attaches the source rather
 * than at whichever caller reached it -- an embedded daemon runs a loop
 * of its own, and a timer on the global default in that arrangement
 * never fires at all, which for this timer means a recording that never
 * ends.
 *
 * Returns: %TRUE if it is now recording
 */
gboolean clawt_teach_recorder_start(ClawtTeachRecorder  *self,
                                    GError             **error);

/**
 * clawt_teach_recorder_stop:
 * @self: a #ClawtTeachRecorder
 * @reason: why it stopped, in a few words for a person
 * @error: (out) (optional): return location for a #GError
 *
 * Stops, saves and reports.
 *
 * A backend that has gone away is not a reason to leave a recording
 * nominally running: the subclass's failure is reported, but the
 * recording is marked stopped and the trace written either way.  A
 * compositor that exited mid-demonstration must produce a usable
 * partial trace, not a recorder nobody can end.
 *
 * Returns: %TRUE if the backend also stopped cleanly
 */
gboolean clawt_teach_recorder_stop(ClawtTeachRecorder  *self,
                                   const gchar         *reason,
                                   GError             **error);

/**
 * clawt_teach_recorder_drain:
 * @self: a #ClawtTeachRecorder
 * @error: (out) (optional): return location for a #GError
 *
 * Takes whatever the backend has buffered, leaving it running.
 *
 * Returns: %TRUE on success
 */
gboolean clawt_teach_recorder_drain(ClawtTeachRecorder  *self,
                                    GError             **error);

/**
 * clawt_teach_recorder_start_async:
 * @self: a #ClawtTeachRecorder
 * @callback: (scope async): called when it has started
 * @user_data: data for @callback
 *
 * clawt_teach_recorder_start() on a worker thread.
 *
 * The answer arrives on the context captured when the recorder was
 * built, for the reason every other async call in this tree names one:
 * a task created from an IPC dispatch would otherwise complete on a
 * loop nobody runs.
 */
void clawt_teach_recorder_start_async(ClawtTeachRecorder  *self,
                                      GAsyncReadyCallback  callback,
                                      gpointer             user_data);

gboolean clawt_teach_recorder_start_finish(ClawtTeachRecorder  *self,
                                           GAsyncResult        *result,
                                           GError             **error);

/**
 * clawt_teach_recorder_stop_async:
 * @self: a #ClawtTeachRecorder
 * @reason: why it stopped
 * @callback: (scope async): called when it has stopped
 * @user_data: data for @callback
 */
void clawt_teach_recorder_stop_async(ClawtTeachRecorder  *self,
                                     const gchar         *reason,
                                     GAsyncReadyCallback  callback,
                                     gpointer             user_data);

gboolean clawt_teach_recorder_stop_finish(ClawtTeachRecorder  *self,
                                          GAsyncResult        *result,
                                          GError             **error);

/**
 * clawt_teach_recorder_note_step:
 * @self: a #ClawtTeachRecorder
 * @step: (transfer full): what happened
 *
 * Records one step, stamping it and attaching a frame if there is one.
 *
 * This is where `skills.teach_max_events` is enforced, and it keeps the
 * **earliest** steps: a demonstration's first minute is a usable prefix
 * of the procedure, while an arbitrary slice out of its middle is not.
 * What was refused is counted and reported rather than hidden -- a
 * silently truncated demonstration teaches half a task and reads as a
 * whole one.
 *
 * Returns: %TRUE if the step was kept
 */
gboolean clawt_teach_recorder_note_step(ClawtTeachRecorder *self,
                                        ClawtTeachStep     *step);

/**
 * clawt_teach_recorder_note_dropped:
 * @self: a #ClawtTeachRecorder
 * @count: how many the backend lost
 *
 * Records losses that happened before the steps reached us -- a
 * compositor's own ring overflowing, which both upstream recorders
 * report on every drain.
 */
void clawt_teach_recorder_note_dropped(ClawtTeachRecorder *self,
                                       guint               count);

/**
 * clawt_teach_recorder_note_suppressed:
 * @self: a #ClawtTeachRecorder
 * @count: how many events were withheld while capture was paused
 */
void clawt_teach_recorder_note_suppressed(ClawtTeachRecorder *self,
                                          guint               count);

/**
 * clawt_teach_recorder_add_caveat:
 * @self: a #ClawtTeachRecorder
 * @text: what a reader of this trace has to be told
 *
 * Carries a backend's own honest limitation into the trace.
 *
 * Called by a subclass at start, so the caveat is on the trace before
 * the first step is -- a recording that failed immediately still says
 * what it would have been able to see.
 */
void clawt_teach_recorder_add_caveat(ClawtTeachRecorder *self,
                                     const gchar        *text);

/**
 * clawt_teach_recorder_set_poll_interval:
 * @self: a #ClawtTeachRecorder
 * @seconds: how often to drain, or 0 never to
 *
 * For a subclass whose backend buffers into a bounded ring.
 *
 * Without this a demonstration nobody was watching would lose its
 * beginning to the compositor's own ring long before anybody asked for
 * it, and the loss would be reported as a number rather than as the
 * steps it was.  #ClawtAgentTraceRecorder leaves it at zero: its steps
 * are pushed in as they happen, so there is nothing buffered anywhere
 * to lose.
 */
void clawt_teach_recorder_set_poll_interval(ClawtTeachRecorder *self,
                                            guint               seconds);

G_END_DECLS
