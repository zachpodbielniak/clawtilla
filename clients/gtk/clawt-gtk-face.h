/*
 * clawt-gtk-face.h - An agent's generated face, and the picker for one
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The geometry is libclawt's (src/agent/clawt-face.h); this only paints
 * it with a GskPath and runs the two motions -- the blink, and the eyes
 * turning toward the pointer while it is over the face's hover area.
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CLAWT_GTK_TYPE_FACE (clawt_gtk_face_get_type())
G_DECLARE_FINAL_TYPE(ClawtGtkFace, clawt_gtk_face, CLAWT_GTK, FACE, GtkWidget)

/**
 * clawt_gtk_face_new:
 * @agent_id: (nullable): whose face; seeds the blink so agents differ
 * @shape: a shape index from clawt_face_resolve()
 * @color: the body colour, `#rgb` or `#rrggbb`; an invalid one is grey
 * @size: width and height, in pixels
 *
 * A face that blinks on its own schedule while it is mapped, and whose
 * eyes follow the pointer while it is over the face itself.  Use
 * clawt_gtk_face_track() to make a larger area -- a sidebar row --
 * count as "over".
 *
 * Returns: (transfer floating): the face
 */
GtkWidget *clawt_gtk_face_new(const gchar *agent_id,
                              gint         shape,
                              const gchar *color,
                              gint         size);

/**
 * clawt_gtk_face_set_shape:
 * @self: a face
 * @shape: a shape index
 *
 * Changes the shape, keeping the pose.
 */
void clawt_gtk_face_set_shape(ClawtGtkFace *self, gint shape);

/**
 * clawt_gtk_face_get_shape:
 * @self: a face
 *
 * Returns: the shape index
 */
gint clawt_gtk_face_get_shape(ClawtGtkFace *self);

/**
 * clawt_gtk_face_set_color:
 * @self: a face
 * @color: (nullable): the body colour; an invalid one is grey
 *
 * Changes the colour; the eyes' colour follows it.
 */
void clawt_gtk_face_set_color(ClawtGtkFace *self, const gchar *color);

/**
 * clawt_gtk_face_set_agent_id:
 * @self: a face
 * @agent_id: (nullable): whose face
 *
 * Changes whose blink schedule the face follows.
 */
void clawt_gtk_face_set_agent_id(ClawtGtkFace *self, const gchar *agent_id);

/**
 * clawt_gtk_face_track:
 * @self: a face
 * @area: a widget containing (or being) the face
 *
 * Makes the eyes follow the pointer while it is anywhere over @area,
 * and return to rest when it leaves.  The pointer is translated into the
 * face's own coordinates, so the eyes look at where it really is.  The
 * controller is tied to @self: when the face goes, it stops.
 */
void clawt_gtk_face_track(ClawtGtkFace *self, GtkWidget *area);

/**
 * clawt_gtk_face_point_at:
 * @self: a face
 * @x: the pointer, in @self's own coordinates
 * @y: likewise
 *
 * What a motion event over the tracked area does: sets the eyes' target
 * toward (@x, @y).  Exposed for clawt_gtk_face_track()'s handlers and
 * for tests.
 */
void clawt_gtk_face_point_at(ClawtGtkFace *self, gdouble x, gdouble y);

/**
 * clawt_gtk_face_look_away:
 * @self: a face
 *
 * What leaving the tracked area does: the eyes go back to rest.
 */
void clawt_gtk_face_look_away(ClawtGtkFace *self);

/**
 * clawt_gtk_face_get_pose:
 * @self: a face
 * @look_x: (out) (optional): where the eyes look now, across
 * @look_y: (out) (optional): where they look now, down
 * @target_x: (out) (optional): where they are heading, across
 * @target_y: (out) (optional): where they are heading, down
 *
 * The eyes' current and target look, for tests.
 */
void clawt_gtk_face_get_pose(ClawtGtkFace *self,
                             gdouble      *look_x,
                             gdouble      *look_y,
                             gdouble      *target_x,
                             gdouble      *target_y);

/**
 * clawt_gtk_face_step:
 * @self: a face
 * @dt_ms: milliseconds to advance
 *
 * One frame of easing toward the target, as the frame clock runs it.
 * Exposed for tests.
 *
 * Returns: %TRUE while the eyes are still moving
 */
gboolean clawt_gtk_face_step(ClawtGtkFace *self, gdouble dt_ms);

/**
 * clawt_gtk_face_is_blink_armed:
 * @self: a face
 *
 * Whether a timer is waiting for the next blink.  Only while mapped
 * and animating: a face nobody can see costs nothing.
 *
 * Returns: %TRUE if armed
 */
gboolean clawt_gtk_face_is_blink_armed(ClawtGtkFace *self);

/**
 * clawt_gtk_faces_set_animate:
 * @animate: whether faces blink and follow the pointer
 *
 * The client-wide switch, from the appearance's `animate_faces` and the
 * desktop's `gtk-enable-animations` together.  Turning it off settles
 * every live face at rest at once; turning it on re-arms their blinks.
 */
void clawt_gtk_faces_set_animate(gboolean animate);

/**
 * clawt_gtk_faces_get_animate:
 *
 * Returns: whether faces move
 */
gboolean clawt_gtk_faces_get_animate(void);

/**
 * clawt_gtk_face_note:
 * @agent_id: an agent's id
 * @face: (nullable): its resolved face, from `agent.list`'s `face`
 *
 * Remembers which face the daemon resolved for an agent, so every place
 * that draws it -- sidebar, transcript, inspector -- draws the same one
 * without each having to carry the field.  %NULL forgets it.
 */
void clawt_gtk_face_note(const gchar *agent_id, const gchar *face);

/**
 * clawt_gtk_face_for:
 * @agent_id: (nullable): an agent's id
 *
 * The face to draw for an agent: what clawt_gtk_face_note() was told,
 * or the automatic face when nothing was (a daemon older than faces,
 * or a sender that is not an agent on the list).
 *
 * Returns: a shape index, or %CLAWT_FACE_INITIALS
 */
gint clawt_gtk_face_for(const gchar *agent_id);

/* ── The picker ──────────────────────────────────────────────────── */

#define CLAWT_GTK_TYPE_FACE_PICKER (clawt_gtk_face_picker_get_type())
G_DECLARE_FINAL_TYPE(ClawtGtkFacePicker, clawt_gtk_face_picker,
                     CLAWT_GTK, FACE_PICKER, GtkBox)

/**
 * clawt_gtk_face_picker_new:
 * @agent_id: (nullable): whose face; its automatic shape and colour are
 *   what "Automatic" shows
 * @face: (nullable): the configured `agents.face`; %NULL or empty for
 *   automatic
 * @color: (nullable): the configured `agents.color`; %NULL or empty for
 *   automatic
 *
 * A large preview that blinks and watches the pointer, a grid of every
 * shape drawn in the chosen colour (plus Automatic and Initials), and a
 * row of colour swatches.  Emits "changed" when either choice changes.
 *
 * Returns: (transfer floating): the picker
 */
GtkWidget *clawt_gtk_face_picker_new(const gchar *agent_id,
                                     const gchar *face,
                                     const gchar *color);

/**
 * clawt_gtk_face_picker_set_agent_id:
 * @self: a picker
 * @agent_id: (nullable): whose face
 *
 * Changes whose automatic face Automatic shows -- the create dialog
 * calls this as the id is typed.
 */
void clawt_gtk_face_picker_set_agent_id(ClawtGtkFacePicker *self,
                                        const gchar        *agent_id);

/**
 * clawt_gtk_face_picker_get_face:
 * @self: a picker
 *
 * Returns: (transfer none): the value for `agents.face`: a shape name,
 *   `initials`, or "" for automatic
 */
const gchar *clawt_gtk_face_picker_get_face(ClawtGtkFacePicker *self);

/**
 * clawt_gtk_face_picker_get_color:
 * @self: a picker
 *
 * Returns: (transfer none): the value for `agents.color`: `#rrggbb`, or
 *   "" for automatic
 */
const gchar *clawt_gtk_face_picker_get_color(ClawtGtkFacePicker *self);

/**
 * clawt_gtk_face_picker_select:
 * @self: a picker
 * @face: (nullable): as clawt_gtk_face_picker_new() takes it
 * @color: (nullable): likewise
 *
 * Selects a face and colour, as a click would, emitting "changed".
 */
void clawt_gtk_face_picker_select(ClawtGtkFacePicker *self,
                                  const gchar        *face,
                                  const gchar        *color);

/**
 * clawt_gtk_face_picker_shuffle:
 * @self: a picker
 *
 * Selects a random shape and palette colour (clawt_face_pick_random()),
 * as the Shuffle button does and as the New agent dialog does when it
 * opens.
 */
void clawt_gtk_face_picker_shuffle(ClawtGtkFacePicker *self);

/**
 * clawt_gtk_face_picker_get_preview:
 * @self: a picker
 *
 * Returns: (transfer none): the large preview face, for tests
 */
ClawtGtkFace *clawt_gtk_face_picker_get_preview(ClawtGtkFacePicker *self);

G_END_DECLS
