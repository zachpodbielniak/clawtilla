/*
 * clawt-teach-trace.h - What was recorded, in order, with its caveats
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A trace is the raw material a skill is written from: an ordered list
 * of steps, optionally a frame beside some of them, and -- this is the
 * part that matters -- the caveats about how it was captured.
 *
 * The caveats travel *with* the trace rather than living in a document,
 * because the trace is what somebody reads.  A demonstration recorded on
 * a Wayland desktop may contain a password: gowl cannot see inside a
 * client's widget tree, so its guard is a deny list of credential
 * applications and title patterns and nothing more.  A reader who has to
 * go and find that out somewhere else is a reader who will not.  So
 * every payload that leaves here carries the sentence, the synthesizer
 * puts it in the draft, and the clients show it beside the trace.
 *
 * Everything here is a pure function of its inputs -- parse, render,
 * save, load -- so the whole shape can be asserted on without a
 * compositor, which is the only way any of this gets tested at all.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

#include "clawt-enums.h"
#include "clawt-types.h"

G_BEGIN_DECLS

/**
 * CLAWT_TEACH_DEFAULT_MAX_SECONDS:
 *
 * `skills.teach_max_seconds`'s default, and the value used when a
 * recorder is built without a configuration to ask.
 */
#define CLAWT_TEACH_DEFAULT_MAX_SECONDS (900)

/**
 * CLAWT_TEACH_DEFAULT_MAX_EVENTS:
 *
 * `skills.teach_max_events`'s default.
 */
#define CLAWT_TEACH_DEFAULT_MAX_EVENTS (20000)

/**
 * CLAWT_TEACH_HOST_DEMO_CAVEAT:
 *
 * What a host demonstration's guard does **not** cover, in gowl's own
 * words, carried into every trace it produces.
 *
 * This is the sentence that must not get lost between layers.  gowl
 * suppresses capture while the session is locked and while the focused
 * window's app-id or title matches its deny list, and that is the whole
 * of it: under Wayland a client's widget tree is private to the client,
 * so there is no equivalent of GNOME Shell's "the key-focus actor is a
 * password entry".  A login form in a browser window whose title says
 * nothing about passwords is recorded.
 */
#define CLAWT_TEACH_HOST_DEMO_CAVEAT \
    "This recording is credential material until you have read it. " \
    "Under Wayland a client's widget tree is private to the client, so " \
    "gowl cannot detect a password field the way GNOME Shell can. It " \
    "suppresses capture while the session is locked and while the " \
    "focused window's app-id or title matches its deny list of " \
    "credential applications -- and that is the whole of the guard. A " \
    "password typed into a form inside an ordinary window the deny list " \
    "does not name IS recorded. Review the steps before this trace is " \
    "stored, shared, or turned into a skill."

/**
 * CLAWT_TEACH_GUEST_DEMO_CAVEAT:
 *
 * The same for a guest demonstration, which has a better guard and not
 * a complete one.
 *
 * GNOME Shell can see that the actor holding key focus is a password
 * entry, and capture pauses when it does -- but that is the *shell's*
 * own actor tree: its lock screen, its polkit and keyring prompts, its
 * search entry.  An application window is a Wayland client there too,
 * and what is inside it is just as private.
 */
#define CLAWT_TEACH_GUEST_DEMO_CAVEAT \
    "This recording is credential material until you have read it. " \
    "Capture pauses by itself while the screen is locked and while a " \
    "password entry belonging to GNOME Shell -- its lock screen, its " \
    "polkit and keyring prompts -- holds the keyboard, and the gaps are " \
    "written into the trace rather than left silent. An application " \
    "window is a Wayland client whose widget tree the compositor cannot " \
    "see into, so a password typed into a form inside one IS recorded. " \
    "Review the steps before this trace is stored, shared, or turned " \
    "into a skill."

/* ── One step ────────────────────────────────────────────────────── */

#define CLAWT_TYPE_TEACH_STEP (clawt_teach_step_get_type())

GType clawt_teach_step_get_type(void) G_GNUC_CONST;

/**
 * clawt_teach_step_new:
 * @kind: what sort of step this is
 * @label: a one-line description, for a person and for the model
 *
 * Returns: (transfer full): the step, with no timestamps yet
 */
ClawtTeachStep *clawt_teach_step_new(ClawtTeachStepKind  kind,
                                     const gchar        *label);

ClawtTeachStep *clawt_teach_step_copy(ClawtTeachStep *self);
void            clawt_teach_step_free(ClawtTeachStep *self);

ClawtTeachStepKind clawt_teach_step_get_kind(ClawtTeachStep *self);
const gchar       *clawt_teach_step_get_label(ClawtTeachStep *self);
const gchar       *clawt_teach_step_get_detail(ClawtTeachStep *self);
const gchar       *clawt_teach_step_get_frame(ClawtTeachStep *self);
gint64             clawt_teach_step_get_wall_us(ClawtTeachStep *self);
gint64             clawt_teach_step_get_offset_us(ClawtTeachStep *self);

void clawt_teach_step_set_detail(ClawtTeachStep *self, const gchar *detail);

/**
 * clawt_teach_step_set_frame:
 * @self: a #ClawtTeachStep
 * @frame: (nullable): a file name **inside the trace's own directory**
 *
 * The picture taken when this step happened.
 *
 * A bare file name rather than a path, so a trace directory copied
 * somewhere else still resolves.  A path would be the trap this tree has
 * already recorded for avatars and attachments: it works on the machine
 * that wrote it and nowhere else.
 */
void clawt_teach_step_set_frame(ClawtTeachStep *self, const gchar *frame);

void clawt_teach_step_set_times(ClawtTeachStep *self,
                                gint64          wall_us,
                                gint64          offset_us);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtTeachStep, clawt_teach_step_free)

/* ── The trace ───────────────────────────────────────────────────── */

#define CLAWT_TYPE_TEACH_TRACE (clawt_teach_trace_get_type())

GType clawt_teach_trace_get_type(void) G_GNUC_CONST;

/**
 * clawt_teach_trace_new:
 * @id: the recording's id, which is also its directory name
 * @source: which recorder made it
 *
 * Returns: (transfer full) (nullable): the trace, or %NULL when @id is
 *   not a usable id
 */
ClawtTeachTrace *clawt_teach_trace_new(const gchar      *id,
                                       ClawtTeachSource  source);

ClawtTeachTrace *clawt_teach_trace_ref(ClawtTeachTrace *self);
void             clawt_teach_trace_unref(ClawtTeachTrace *self);

const gchar      *clawt_teach_trace_get_id(ClawtTeachTrace *self);
ClawtTeachSource  clawt_teach_trace_get_source(ClawtTeachTrace *self);
const gchar      *clawt_teach_trace_get_agent_id(ClawtTeachTrace *self);
const gchar      *clawt_teach_trace_get_goal(ClawtTeachTrace *self);
const gchar      *clawt_teach_trace_get_directory(ClawtTeachTrace *self);
const gchar      *clawt_teach_trace_get_stop_reason(ClawtTeachTrace *self);
gint64            clawt_teach_trace_get_started_at(ClawtTeachTrace *self);
gint64            clawt_teach_trace_get_ended_at(ClawtTeachTrace *self);
guint             clawt_teach_trace_get_dropped(ClawtTeachTrace *self);
guint             clawt_teach_trace_get_suppressed(ClawtTeachTrace *self);

void clawt_teach_trace_set_agent_id(ClawtTeachTrace *self, const gchar *id);
void clawt_teach_trace_set_goal(ClawtTeachTrace *self, const gchar *goal);
void clawt_teach_trace_set_directory(ClawtTeachTrace *self,
                                     const gchar     *directory);
void clawt_teach_trace_set_stop_reason(ClawtTeachTrace *self,
                                       const gchar     *reason);
void clawt_teach_trace_set_started_at(ClawtTeachTrace *self, gint64 stamp);
void clawt_teach_trace_set_ended_at(ClawtTeachTrace *self, gint64 stamp);
void clawt_teach_trace_add_dropped(ClawtTeachTrace *self, guint count);
void clawt_teach_trace_add_suppressed(ClawtTeachTrace *self, guint count);

/**
 * clawt_teach_trace_add_step:
 * @self: a #ClawtTeachTrace
 * @step: (transfer full): the step
 *
 * Appends a step, unconditionally.
 *
 * The bound lives on #ClawtTeachRecorder rather than here, because the
 * bound is about a *recording* and this type is also what a saved trace
 * is loaded back into -- refusing there would make a trace that reached
 * its cap unreadable afterwards.
 */
void clawt_teach_trace_add_step(ClawtTeachTrace *self, ClawtTeachStep *step);

/**
 * clawt_teach_trace_get_steps:
 * @self: a #ClawtTeachTrace
 *
 * Returns: (transfer none) (element-type ClawtTeachStep): the steps, in
 *   the order they happened
 */
GPtrArray *clawt_teach_trace_get_steps(ClawtTeachTrace *self);

/**
 * clawt_teach_trace_add_caveat:
 * @self: a #ClawtTeachTrace
 * @text: what a reader has to know about how this was captured
 *
 * Records a caveat, once.
 *
 * Deduplicated because a recorder adds its caveat at start and every
 * drain reports it again -- and a caveat repeated forty times is one
 * nobody reads, which defeats the whole reason it is carried here
 * rather than left in a document.
 */
void clawt_teach_trace_add_caveat(ClawtTeachTrace *self, const gchar *text);

/**
 * clawt_teach_trace_get_caveats:
 * @self: a #ClawtTeachTrace
 *
 * Returns: (transfer none) (element-type utf8): the caveats
 */
GPtrArray *clawt_teach_trace_get_caveats(ClawtTeachTrace *self);

/**
 * clawt_teach_trace_count_frames:
 * @self: a #ClawtTeachTrace
 *
 * Returns: how many steps have a picture beside them
 */
guint clawt_teach_trace_count_frames(ClawtTeachTrace *self);

/**
 * clawt_teach_trace_render:
 * @self: a #ClawtTeachTrace
 * @max_steps: how many steps to include, or 0 for all of them
 *
 * The trace as text, which is what the model is shown.
 *
 * Rendered here rather than in the synthesizer so that what a person
 * reads in a client and what the model reads are the same rendering.
 * Two of them would differ exactly once, on the step somebody was
 * arguing about.
 *
 * Returns: (transfer full): the rendering
 */
gchar *clawt_teach_trace_render(ClawtTeachTrace *self, guint max_steps);

/**
 * clawt_teach_trace_to_json:
 * @self: a #ClawtTeachTrace
 * @with_steps: whether to include every step, or only the counts
 *
 * Returns: (transfer full): the trace as a JSON object node
 */
JsonNode *clawt_teach_trace_to_json(ClawtTeachTrace *self,
                                    gboolean         with_steps);

/**
 * clawt_teach_trace_from_json:
 * @node: an object node written by clawt_teach_trace_to_json()
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the trace
 */
ClawtTeachTrace *clawt_teach_trace_from_json(JsonNode  *node,
                                             GError   **error);

/**
 * clawt_teach_trace_save:
 * @self: a #ClawtTeachTrace
 * @error: (out) (optional): return location for a #GError
 *
 * Writes `trace.json` into the trace's own directory.
 *
 * Refuses when no directory has been set rather than choosing one: a
 * recording that wrote itself somewhere convenient is a recording of
 * somebody's keystrokes in a place they did not agree to.
 *
 * Returns: %TRUE on success
 */
gboolean clawt_teach_trace_save(ClawtTeachTrace *self, GError **error);

/**
 * clawt_teach_trace_load:
 * @directory: a trace directory holding a `trace.json`
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the trace
 */
ClawtTeachTrace *clawt_teach_trace_load(const gchar  *directory,
                                        GError      **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtTeachTrace, clawt_teach_trace_unref)

G_END_DECLS
