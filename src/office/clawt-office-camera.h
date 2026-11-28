/*
 * clawt-office-camera.h - The office camera and the keys that drive it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * One camera for the standalone window, the GTK embed and the lrg
 * Office page: a point on the floor, a yaw, a pitch and a distance,
 * smoothed the same way whichever frontend fed the input.  No toolkit
 * and no GL, so the tests run the same code the scene does.
 *
 * Keys are raylib's numbers (graylib's GrlKey uses the same values).
 * The binding table is the only place that names them.  A GDK keyval
 * becomes one of those numbers here too, so the GTK page and the test
 * agree on what is forwarded and what GTK keeps.
 */

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#ifndef CLAWT_OFFICE_CAMERA_H
#define CLAWT_OFFICE_CAMERA_H

#include <glib.h>
#include <glib-object.h>

G_BEGIN_DECLS

/**
 * CLAWT_OFFICE_CAMERA_PITCH_MIN:
 *
 * The shallowest the camera may look down, in radians from the
 * horizontal.  Below this the eye would meet the floor.
 */
#define CLAWT_OFFICE_CAMERA_PITCH_MIN (0.08f)

/**
 * CLAWT_OFFICE_CAMERA_PITCH_MAX:
 *
 * The steepest pitch, short of straight down, so the camera cannot
 * flip past it.
 */
#define CLAWT_OFFICE_CAMERA_PITCH_MAX (1.52f)

/**
 * CLAWT_OFFICE_CAMERA_TARGET_MARGIN:
 *
 * How far past the world footprint the aim may sit, in tiles.
 */
#define CLAWT_OFFICE_CAMERA_TARGET_MARGIN (4.0f)

/**
 * CLAWT_OFFICE_CAMERA_DIST_MIN:
 *
 * The nearest the camera may come, in tiles along the view.
 */
#define CLAWT_OFFICE_CAMERA_DIST_MIN (1.5f)

/**
 * CLAWT_OFFICE_CAMERA_DIST_MAX:
 *
 * The furthest the camera may pull back, in tiles along the view.
 * Far enough to stand past a Fit of the shipped offices.
 */
#define CLAWT_OFFICE_CAMERA_DIST_MAX (240.0f)

/**
 * CLAWT_OFFICE_CAMERA_SMOOTH_TIME:
 *
 * The critically damped smooth time, in seconds.  A slow frame takes
 * the same sized step as the frames that would have filled it.
 */
#define CLAWT_OFFICE_CAMERA_SMOOTH_TIME (0.10f)

/**
 * CLAWT_OFFICE_CAMERA_MOVE_SPEED:
 *
 * How fast W/A/S/D move the aim, in tiles per second.
 */
#define CLAWT_OFFICE_CAMERA_MOVE_SPEED (14.0f)

/**
 * CLAWT_OFFICE_CAMERA_FAST_SCALE:
 *
 * What Shift multiplies the move and the turn by.
 */
#define CLAWT_OFFICE_CAMERA_FAST_SCALE (3.0f)

/**
 * CLAWT_OFFICE_CAMERA_TURN_SPEED:
 *
 * How fast Q and E turn, in radians per second.
 */
#define CLAWT_OFFICE_CAMERA_TURN_SPEED (1.6f)

/**
 * CLAWT_OFFICE_CAMERA_LOOK_RAD_PER_PX:
 *
 * How far a pixel of right-drag turns the camera, in radians.
 */
#define CLAWT_OFFICE_CAMERA_LOOK_RAD_PER_PX (0.005f)

/**
 * CLAWT_OFFICE_CAMERA_DOLLY_STEP:
 *
 * The wheel's exponent: one notch multiplies the distance by
 * exp(-step) when the notch is positive (in).
 */
#define CLAWT_OFFICE_CAMERA_DOLLY_STEP (0.18f)

/**
 * CLAWT_OFFICE_CAMERA_PAN_PX_PER_S:
 *
 * How fast the 2D view pans under W/A/S/D, in screen pixels per second.
 */
#define CLAWT_OFFICE_CAMERA_PAN_PX_PER_S (480.0f)

/**
 * CLAWT_OFFICE_CAMERA_ZOOM_MIN:
 *
 * The closest a 2D wheel zoom may come, as a scale on the map.
 */
#define CLAWT_OFFICE_CAMERA_ZOOM_MIN (0.15f)

/**
 * CLAWT_OFFICE_CAMERA_ZOOM_MAX:
 *
 * The furthest a 2D wheel zoom may pull back.
 */
#define CLAWT_OFFICE_CAMERA_ZOOM_MAX (8.0f)

/**
 * CLAWT_OFFICE_CAMERA_ZOOM_STEP:
 *
 * The 2D wheel's exponent, per notch.
 */
#define CLAWT_OFFICE_CAMERA_ZOOM_STEP (0.22f)

/**
 * CLAWT_OFFICE_CAMERA_DOUBLE_CLICK_S:
 *
 * Two presses this close in time, and within
 * %CLAWT_OFFICE_CAMERA_DOUBLE_CLICK_PX, are one double-click.
 */
#define CLAWT_OFFICE_CAMERA_DOUBLE_CLICK_S (0.35f)

/**
 * CLAWT_OFFICE_CAMERA_DOUBLE_CLICK_PX:
 *
 * How far apart the two presses of a double-click may be, in pixels.
 */
#define CLAWT_OFFICE_CAMERA_DOUBLE_CLICK_PX (8.0f)

/**
 * CLAWT_OFFICE_CAMERA_YAW_HOME:
 *
 * The bearing Fit starts at.  The same 0.65 rad the orbit camera used,
 * so an untouched office still looks the way it did.
 */
#define CLAWT_OFFICE_CAMERA_YAW_HOME (0.65f)

/* Raylib key codes.  GrlKey in graylib uses the same numbers. */
/**
 * CLAWT_OFFICE_KEY_A:
 *
 * raylib KEY_A, and the strafe-left binding.
 */
#define CLAWT_OFFICE_KEY_A (65)
/**
 * CLAWT_OFFICE_KEY_D:
 *
 * raylib KEY_D, and the strafe-right binding.
 */
#define CLAWT_OFFICE_KEY_D (68)
/**
 * CLAWT_OFFICE_KEY_E:
 *
 * raylib KEY_E, and the turn-right binding.
 */
#define CLAWT_OFFICE_KEY_E (69)
/**
 * CLAWT_OFFICE_KEY_F:
 *
 * raylib KEY_F, and a reset to Fit.
 */
#define CLAWT_OFFICE_KEY_F (70)
/**
 * CLAWT_OFFICE_KEY_Q:
 *
 * raylib KEY_Q, and the turn-left binding.
 */
#define CLAWT_OFFICE_KEY_Q (81)
/**
 * CLAWT_OFFICE_KEY_S:
 *
 * raylib KEY_S, and the move-back binding.
 */
#define CLAWT_OFFICE_KEY_S (83)
/**
 * CLAWT_OFFICE_KEY_W:
 *
 * raylib KEY_W, and the move-forward binding.
 */
#define CLAWT_OFFICE_KEY_W (87)
/**
 * CLAWT_OFFICE_KEY_ESCAPE:
 *
 * raylib KEY_ESCAPE.  Forwarded so the embed can close the usage
 * panel.  Not a camera binding.
 */
#define CLAWT_OFFICE_KEY_ESCAPE (256)
/**
 * CLAWT_OFFICE_KEY_RIGHT:
 *
 * raylib KEY_RIGHT.  Mirrors D.
 */
#define CLAWT_OFFICE_KEY_RIGHT (262)
/**
 * CLAWT_OFFICE_KEY_LEFT:
 *
 * raylib KEY_LEFT.  Mirrors A.
 */
#define CLAWT_OFFICE_KEY_LEFT (263)
/**
 * CLAWT_OFFICE_KEY_DOWN:
 *
 * raylib KEY_DOWN.  Mirrors S.
 */
#define CLAWT_OFFICE_KEY_DOWN (264)
/**
 * CLAWT_OFFICE_KEY_UP:
 *
 * raylib KEY_UP.  Mirrors W.
 */
#define CLAWT_OFFICE_KEY_UP (265)
/**
 * CLAWT_OFFICE_KEY_HOME:
 *
 * raylib KEY_HOME, and a reset to Fit.
 */
#define CLAWT_OFFICE_KEY_HOME (268)
/**
 * CLAWT_OFFICE_KEY_LEFT_SHIFT:
 *
 * raylib KEY_LEFT_SHIFT.  Held, movement and turning are
 * %CLAWT_OFFICE_CAMERA_FAST_SCALE times quicker.
 */
#define CLAWT_OFFICE_KEY_LEFT_SHIFT (340)
/**
 * CLAWT_OFFICE_KEY_RIGHT_SHIFT:
 *
 * raylib KEY_RIGHT_SHIFT.  The same as the left one.
 */
#define CLAWT_OFFICE_KEY_RIGHT_SHIFT (344)

typedef struct _ClawtOfficeCamera ClawtOfficeCamera;

#define CLAWT_TYPE_OFFICE_CAMERA (clawt_office_camera_get_type())

/**
 * ClawtOfficeBindAction:
 * @CLAWT_OFFICE_BIND_NONE: not a camera key
 * @CLAWT_OFFICE_BIND_FORWARD: W or Up
 * @CLAWT_OFFICE_BIND_BACK: S or Down
 * @CLAWT_OFFICE_BIND_LEFT: A or Left
 * @CLAWT_OFFICE_BIND_RIGHT: D or Right
 * @CLAWT_OFFICE_BIND_TURN_LEFT: Q
 * @CLAWT_OFFICE_BIND_TURN_RIGHT: E
 * @CLAWT_OFFICE_BIND_FASTER: either Shift
 * @CLAWT_OFFICE_BIND_FIT: F or Home
 *
 * What one row of the binding table does.
 */
typedef enum {
    CLAWT_OFFICE_BIND_NONE = 0,
    CLAWT_OFFICE_BIND_FORWARD,
    CLAWT_OFFICE_BIND_BACK,
    CLAWT_OFFICE_BIND_LEFT,
    CLAWT_OFFICE_BIND_RIGHT,
    CLAWT_OFFICE_BIND_TURN_LEFT,
    CLAWT_OFFICE_BIND_TURN_RIGHT,
    CLAWT_OFFICE_BIND_FASTER,
    CLAWT_OFFICE_BIND_FIT
} ClawtOfficeBindAction;

/**
 * ClawtOfficeInput:
 * @dt: seconds since the previous sample
 * @view_w: the view, in pixels
 * @view_h: the view, in pixels
 * @x: the pointer, in view pixels
 * @y: the pointer, in view pixels
 * @pointer_valid: whether @x and @y are over the view
 * @dx: how far the pointer moved this sample, in pixels
 * @dy: likewise; positive is down
 * @left_down: the left button is held
 * @right_down: the right button is held
 * @middle_down: the middle button is held
 * @left_pressed: the left button went down this sample
 * @wheel: wheel notches, positive zooms in
 * @move_forward: W or Up is held
 * @move_back: S or Down is held
 * @move_left: A or Left is held
 * @move_right: D or Right is held
 * @turn_left: Q is held
 * @turn_right: E is held
 * @shift: either Shift is held
 * @reset_fit: F or Home went down this sample
 *
 * One sample of input, filled by a frontend.  Zero it, then call
 * clawt_office_input_note_key() for each row of the binding table.
 * The scene reads it; the frontends do not decide what a drag does.
 */
typedef struct {
    gfloat   dt;
    gfloat   view_w;
    gfloat   view_h;
    gfloat   x;
    gfloat   y;
    gboolean pointer_valid;
    gfloat   dx;
    gfloat   dy;
    gboolean left_down;
    gboolean right_down;
    gboolean middle_down;
    gboolean left_pressed;
    gfloat   wheel;
    gboolean move_forward;
    gboolean move_back;
    gboolean move_left;
    gboolean move_right;
    gboolean turn_left;
    gboolean turn_right;
    gboolean shift;
    gboolean reset_fit;
} ClawtOfficeInput;

GType clawt_office_camera_get_type(void) G_GNUC_CONST;

/**
 * clawt_office_camera_new:
 *
 * Returns: (transfer full): a camera aimed at the origin, at the home
 *   bearing and the Fit pitch
 */
ClawtOfficeCamera *clawt_office_camera_new(void);

/**
 * clawt_office_camera_copy:
 * @self: (nullable): a camera
 *
 * Returns: (transfer full) (nullable): a camera with the same pose and
 *   the same spring
 */
ClawtOfficeCamera *clawt_office_camera_copy(ClawtOfficeCamera *self);

/**
 * clawt_office_camera_free:
 * @self: (nullable): a camera
 */
void clawt_office_camera_free(ClawtOfficeCamera *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtOfficeCamera, clawt_office_camera_free)

/**
 * clawt_office_camera_pitch_fit:
 *
 * The pitch at which the eye sits
 * %CLAWT_OFFICE_VIEW_3D_LIFT times as high as it is far across the
 * floor, which is where Fit puts it.
 *
 * Returns: radians down from the horizontal
 */
gfloat clawt_office_camera_pitch_fit(void);

/**
 * clawt_office_camera_set_bounds:
 * @self: a camera
 * @min_x: the footprint's low x, in tiles
 * @max_x: its high x
 * @min_z: its low z
 * @max_z: its high z
 *
 * The aim is kept inside this rectangle plus
 * %CLAWT_OFFICE_CAMERA_TARGET_MARGIN.
 */
void clawt_office_camera_set_bounds(ClawtOfficeCamera *self,
                                    gfloat             min_x,
                                    gfloat             max_x,
                                    gfloat             min_z,
                                    gfloat             max_z);

/**
 * clawt_office_camera_set_desired_target:
 * @self: a camera
 * @x: where to aim, in tiles
 * @z: and z
 *
 * Clamped to the bounds.  The shown aim eases toward it.
 */
void clawt_office_camera_set_desired_target(ClawtOfficeCamera *self,
                                            gfloat             x,
                                            gfloat             z);

/**
 * clawt_office_camera_set_desired_yaw:
 * @self: a camera
 * @yaw: radians.  The eye stands at (cos @yaw, sin @yaw) from the aim
 *   across the floor, the same sense the old orbit used
 */
void clawt_office_camera_set_desired_yaw(ClawtOfficeCamera *self, gfloat yaw);

/**
 * clawt_office_camera_set_desired_pitch:
 * @self: a camera
 * @pitch: radians down from the horizontal, clamped
 */
void clawt_office_camera_set_desired_pitch(ClawtOfficeCamera *self, gfloat pitch);

/**
 * clawt_office_camera_set_desired_distance:
 * @self: a camera
 * @distance: tiles along the view, from the eye to the aim, clamped
 */
void clawt_office_camera_set_desired_distance(ClawtOfficeCamera *self,
                                              gfloat             distance);

/**
 * clawt_office_camera_snap:
 * @self: a camera
 *
 * The shown pose becomes the desired one, with no velocity left.
 * Fit, a follow that has just started, and `--camera` use this so the
 * first frame is not an ease in from the origin.
 */
void clawt_office_camera_snap(ClawtOfficeCamera *self);

/**
 * clawt_office_camera_snap_pose:
 * @self: a camera
 * @x: the aim, in tiles
 * @z: and z
 * @yaw: radians
 * @pitch: radians down from the horizontal
 * @distance: tiles along the view
 *
 * Sets the desired pose and snaps to it.
 */
void clawt_office_camera_snap_pose(ClawtOfficeCamera *self,
                                   gfloat             x,
                                   gfloat             z,
                                   gfloat             yaw,
                                   gfloat             pitch,
                                   gfloat             distance);

/**
 * clawt_office_camera_look:
 * @self: a camera
 * @dx_px: pointer movement, pixels; positive yaws the way a drag to
 *   the right always has
 * @dy_px: positive looks further down
 */
void clawt_office_camera_look(ClawtOfficeCamera *self, gfloat dx_px, gfloat dy_px);

/**
 * clawt_office_camera_pan:
 * @self: a camera
 * @dx_px: pointer movement, pixels
 * @dy_px: positive is down the view
 * @view_h: the view's height, in pixels
 * @fovy: the vertical field of view, in degrees
 *
 * Grabs the floor: the aim moves opposite the drag, by about one
 * pixel of floor per pixel of pointer.
 */
void clawt_office_camera_pan(ClawtOfficeCamera *self,
                             gfloat             dx_px,
                             gfloat             dy_px,
                             gfloat             view_h,
                             gfloat             fovy);

/**
 * clawt_office_camera_move:
 * @self: a camera
 * @forward: +1 along the facing, -1 back, 0 for neither
 * @strafe: +1 to the camera's right, -1 to its left
 * @dt: seconds
 * @fast: %TRUE while Shift is held
 */
void clawt_office_camera_move(ClawtOfficeCamera *self,
                              gfloat             forward,
                              gfloat             strafe,
                              gfloat             dt,
                              gboolean           fast);

/**
 * clawt_office_camera_turn:
 * @self: a camera
 * @dir: +1 turns left (Q, the old left arrow), -1 turns right
 * @dt: seconds
 * @fast: %TRUE while Shift is held
 */
void clawt_office_camera_turn(ClawtOfficeCamera *self,
                              gfloat             dir,
                              gfloat             dt,
                              gboolean           fast);

/**
 * clawt_office_camera_dolly:
 * @self: a camera
 * @wheel: notches, positive comes closer
 * @has_anchor: whether @anchor_x and @anchor_z are a floor point
 * @anchor_x: the floor point that should stay under the cursor
 * @anchor_z: and z
 *
 * Eases the distance.  With an anchor, the aim slides so that point
 * stays put on screen.
 */
void clawt_office_camera_dolly(ClawtOfficeCamera *self,
                               gfloat             wheel,
                               gboolean           has_anchor,
                               gfloat             anchor_x,
                               gfloat             anchor_z);

/**
 * clawt_office_camera_step:
 * @self: a camera
 * @dt: seconds since the previous step.  A large @dt does not step
 *   past the desired pose
 *
 * Critically damped on the aim, the yaw, the pitch and the distance.
 *
 * Returns: %TRUE while the shown pose is still catching up
 */
gboolean clawt_office_camera_step(ClawtOfficeCamera *self, gfloat dt);

/**
 * clawt_office_camera_eye:
 * @self: a camera
 * @x: (out): the eye, in tiles
 * @y: (out): height above the floor
 * @z: (out): and z
 *
 * From the shown pose.  At the Fit pitch and a distance of
 * reach * hypot(1, %CLAWT_OFFICE_VIEW_3D_LIFT) this is the old Fit eye.
 */
void clawt_office_camera_eye(ClawtOfficeCamera *self,
                             gfloat            *x,
                             gfloat            *y,
                             gfloat            *z);

/**
 * clawt_office_camera_target:
 * @self: a camera
 * @x: (out): the shown aim, in tiles
 * @z: (out): and z
 */
void clawt_office_camera_target(ClawtOfficeCamera *self, gfloat *x, gfloat *z);

/**
 * clawt_office_camera_get_yaw:
 * @self: a camera
 *
 * Returns: the shown yaw, in radians
 */
gfloat clawt_office_camera_get_yaw(ClawtOfficeCamera *self);

/**
 * clawt_office_camera_get_pitch:
 * @self: a camera
 *
 * Returns: the shown pitch, in radians down from the horizontal
 */
gfloat clawt_office_camera_get_pitch(ClawtOfficeCamera *self);

/**
 * clawt_office_camera_get_distance:
 * @self: a camera
 *
 * Returns: the shown distance, in tiles along the view
 */
gfloat clawt_office_camera_get_distance(ClawtOfficeCamera *self);

/**
 * clawt_office_camera_get_desired_yaw:
 * @self: a camera
 *
 * Returns: the yaw being eased toward
 */
gfloat clawt_office_camera_get_desired_yaw(ClawtOfficeCamera *self);

/**
 * clawt_office_camera_get_desired_pitch:
 * @self: a camera
 *
 * Returns: the pitch being eased toward, already clamped
 */
gfloat clawt_office_camera_get_desired_pitch(ClawtOfficeCamera *self);

/**
 * clawt_office_camera_get_desired_distance:
 * @self: a camera
 *
 * Returns: the distance being eased toward, already clamped
 */
gfloat clawt_office_camera_get_desired_distance(ClawtOfficeCamera *self);

/**
 * clawt_office_camera_get_desired_target:
 * @self: a camera
 * @x: (out): the aim being eased toward
 * @z: (out): and z
 */
void clawt_office_camera_get_desired_target(ClawtOfficeCamera *self,
                                            gfloat            *x,
                                            gfloat            *z);

/**
 * clawt_office_camera_floor_at:
 * @self: a camera
 * @px: a pixel, from the left of the view
 * @py: from the top
 * @view_w: the view's width
 * @view_h: its height
 * @fovy: the vertical field of view, in degrees
 * @x: (out): where that pixel's ray meets the floor
 * @z: (out): and z
 *
 * The inverse of clawt_office_view_project_3d() for a point on the
 * floor, using the shown pose.
 *
 * Returns: %FALSE when the ray does not meet the floor in front of
 *   the camera, leaving @x and @z alone
 */
gboolean clawt_office_camera_floor_at(ClawtOfficeCamera *self,
                                      gfloat             px,
                                      gfloat             py,
                                      gfloat             view_w,
                                      gfloat             view_h,
                                      gfloat             fovy,
                                      gfloat            *x,
                                      gfloat            *z);

/**
 * clawt_office_camera_zoom_around:
 * @centre_x: (inout): the 2D view's centre, in map pixels
 * @centre_y: (inout): and y
 * @old_zoom: the scale before the wheel
 * @new_zoom: the scale after it
 * @cursor_x: the pointer, in view pixels
 * @cursor_y: and y
 * @view_w: the view's width
 * @view_h: its height
 *
 * Keeps the map point under the cursor fixed while the zoom changes.
 */
void clawt_office_camera_zoom_around(gfloat *centre_x,
                                     gfloat *centre_y,
                                     gfloat  old_zoom,
                                     gfloat  new_zoom,
                                     gfloat  cursor_x,
                                     gfloat  cursor_y,
                                     gfloat  view_w,
                                     gfloat  view_h);

/**
 * clawt_office_camera_zoom_factor:
 * @zoom: the scale now
 * @wheel: notches, positive zooms in
 *
 * Returns: @zoom times exp(@wheel * %CLAWT_OFFICE_CAMERA_ZOOM_STEP),
 *   clamped to the 2D zoom limits
 */
gfloat clawt_office_camera_zoom_factor(gfloat zoom, gfloat wheel);

/**
 * clawt_office_camera_double_click:
 * @since: seconds since the previous left press
 * @dx: how far this press is from that one, in pixels
 * @dy: and y
 *
 * Returns: %TRUE when the two presses are one double-click
 */
gboolean clawt_office_camera_double_click(gfloat since, gfloat dx, gfloat dy);

/**
 * clawt_office_binding_action:
 * @key: a raylib key code
 *
 * Returns: what @key does, or %CLAWT_OFFICE_BIND_NONE
 */
ClawtOfficeBindAction clawt_office_binding_action(guint key);

/**
 * clawt_office_binding_count:
 *
 * Returns: how many rows the binding table has
 */
guint clawt_office_binding_count(void);

/**
 * clawt_office_binding_key:
 * @index: a row, from 0 and below clawt_office_binding_count()
 *
 * Returns: that row's raylib key code, or 0 when @index is past the end
 */
guint clawt_office_binding_key(guint index);

/**
 * clawt_office_key_from_gdk:
 * @keyval: a GDK keyval
 *
 * The raylib code to forward while the office drawing has focus.
 * Letters match either case.  Return, Tab and U are 0: GTK keeps them.
 *
 * Returns: a raylib key code, or 0 when the office does not take the key
 */
guint clawt_office_key_from_gdk(guint keyval);

/**
 * clawt_office_modifiers_from_gdk:
 * @state: a #GdkModifierType, as a number
 *
 * GDK's mask is not the frame protocol's.  Control is bit 2 there and
 * bit 1 on the wire.  This maps shift, control, alt and super.
 *
 * Returns: the protocol's modifier bits (shift 1, control 2, alt 4, super 8)
 */
guint clawt_office_modifiers_from_gdk(guint state);

/**
 * clawt_office_input_note_key:
 * @input: a sample, zeroed by the caller for this frame
 * @key: a raylib key code
 * @pressed: the key went down this sample
 * @down: the key is held
 *
 * ORs the binding into @input.  A Fit binding sets @reset_fit only on
 * the press.  Shift only sets @shift, so the caller zeroes @input
 * first and both Shift keys can be noted.
 */
void clawt_office_input_note_key(ClawtOfficeInput *input,
                                 guint             key,
                                 gboolean          pressed,
                                 gboolean          down);

G_END_DECLS

#endif /* CLAWT_OFFICE_CAMERA_H */
