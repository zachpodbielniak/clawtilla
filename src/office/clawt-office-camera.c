/*
 * clawt-office-camera.c - The office camera and the keys that drive it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * target + yaw + pitch + distance, instead of an orbit around the map
 * centre.  Pitch is down from the horizontal.  The eye is
 *
 *   target + (cos(yaw) cos(pitch) distance,
 *             sin(pitch) distance,
 *             sin(yaw) cos(pitch) distance)
 *
 * so at pitch = atan(CLAWT_OFFICE_VIEW_3D_LIFT) and a distance of
 * reach * hypot(1, lift) the eye matches the old Fit camera, whose
 * height was reach * lift and whose ground distance was reach.
 *
 * Desired and shown are separate.  Input writes desired (and, for a
 * dolly toward a floor point, slides both so the spring does not pull
 * the aim back).  step() eases shown with a critically damped spring
 * whose step depends on dt, and a slow frame cannot pass the target.
 */

#include "clawtilla.h"

#include <math.h>

G_DEFINE_BOXED_TYPE(ClawtOfficeCamera, clawt_office_camera,
                    clawt_office_camera_copy, clawt_office_camera_free)

/* How flat a pitch may be before a floor pan stops growing. */
#define PAN_SIN_FLOOR (0.20f)

/* Close enough to rest that the host may stop drawing. */
#define REST_POS (0.0005f)
#define REST_ANG (0.0001f)
#define REST_VEL (0.001f)

struct _ClawtOfficeCamera {
    gfloat   des_x;
    gfloat   des_z;
    gfloat   des_yaw;
    gfloat   des_pitch;
    gfloat   des_dist;
    gfloat   shown_x;
    gfloat   shown_z;
    gfloat   shown_yaw;
    gfloat   shown_pitch;
    gfloat   shown_dist;
    gfloat   vel_x;
    gfloat   vel_z;
    gfloat   vel_yaw;
    gfloat   vel_pitch;
    gfloat   vel_dist;
    gfloat   min_x;
    gfloat   max_x;
    gfloat   min_z;
    gfloat   max_z;
    gboolean have_bounds;
};

typedef struct {
    guint                 key;
    ClawtOfficeBindAction action;
} Bind;

/* The one table.  Arrows mirror WASD.  Both Shifts are the same. */
static const Bind bindings[] = {
    { CLAWT_OFFICE_KEY_W,           CLAWT_OFFICE_BIND_FORWARD },
    { CLAWT_OFFICE_KEY_UP,          CLAWT_OFFICE_BIND_FORWARD },
    { CLAWT_OFFICE_KEY_S,           CLAWT_OFFICE_BIND_BACK },
    { CLAWT_OFFICE_KEY_DOWN,        CLAWT_OFFICE_BIND_BACK },
    { CLAWT_OFFICE_KEY_A,           CLAWT_OFFICE_BIND_LEFT },
    { CLAWT_OFFICE_KEY_LEFT,        CLAWT_OFFICE_BIND_LEFT },
    { CLAWT_OFFICE_KEY_D,           CLAWT_OFFICE_BIND_RIGHT },
    { CLAWT_OFFICE_KEY_RIGHT,       CLAWT_OFFICE_BIND_RIGHT },
    { CLAWT_OFFICE_KEY_Q,           CLAWT_OFFICE_BIND_TURN_LEFT },
    { CLAWT_OFFICE_KEY_E,           CLAWT_OFFICE_BIND_TURN_RIGHT },
    { CLAWT_OFFICE_KEY_LEFT_SHIFT,  CLAWT_OFFICE_BIND_FASTER },
    { CLAWT_OFFICE_KEY_RIGHT_SHIFT, CLAWT_OFFICE_BIND_FASTER },
    { CLAWT_OFFICE_KEY_F,           CLAWT_OFFICE_BIND_FIT },
    { CLAWT_OFFICE_KEY_HOME,        CLAWT_OFFICE_BIND_FIT }
};

/* GDK keyvals, as numbers, so this file does not include GDK. */
#define GDK_W (0x0077u)
#define GDK_A (0x0061u)
#define GDK_S (0x0073u)
#define GDK_D (0x0064u)
#define GDK_Q (0x0071u)
#define GDK_E (0x0065u)
#define GDK_F (0x0066u)
#define GDK_ESCAPE (0xff1bu)
#define GDK_HOME (0xff50u)
#define GDK_LEFT (0xff51u)
#define GDK_UP (0xff52u)
#define GDK_RIGHT (0xff53u)
#define GDK_DOWN (0xff54u)
#define GDK_SHIFT_L (0xffe1u)
#define GDK_SHIFT_R (0xffe2u)

/* GdkModifierType.  Control is bit 2, not the protocol's bit 1. */
#define GDK_SHIFT_BIT   (1u << 0)
#define GDK_CONTROL_BIT (1u << 2)
#define GDK_ALT_BIT     (1u << 3)
#define GDK_SUPER_BIT   (1u << 26)

static gfloat
clamp_pitch(gfloat pitch)
{
    return CLAMP(pitch, CLAWT_OFFICE_CAMERA_PITCH_MIN, CLAWT_OFFICE_CAMERA_PITCH_MAX);
}

static gfloat
clamp_dist(gfloat distance)
{
    return CLAMP(distance, CLAWT_OFFICE_CAMERA_DIST_MIN, CLAWT_OFFICE_CAMERA_DIST_MAX);
}

static void
clamp_target(ClawtOfficeCamera *self, gfloat *x, gfloat *z)
{
    gfloat margin;

    if (!self->have_bounds)
        return;
    margin = CLAWT_OFFICE_CAMERA_TARGET_MARGIN;
    *x = CLAMP(*x, self->min_x - margin, self->max_x + margin);
    *z = CLAMP(*z, self->min_z - margin, self->max_z + margin);
}

/*
 * Unity's SmoothDamp, without a speed cap.  The last test stops a long
 * dt from stepping past the target, which the polynomial on its own can.
 */
static gfloat
smooth_damp(gfloat current, gfloat target, gfloat *vel, gfloat dt)
{
    gfloat smooth;
    gfloat omega;
    gfloat x;
    gfloat exp;
    gfloat change;
    gfloat temp;
    gfloat output;

    if (!(dt > 0.0f))
        return current;
    smooth = CLAWT_OFFICE_CAMERA_SMOOTH_TIME;
    if (smooth < 0.0001f)
        smooth = 0.0001f;
    omega = 2.0f / smooth;
    x = omega * dt;
    exp = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
    change = current - target;
    temp = (*vel + omega * change) * dt;
    *vel = (*vel - omega * temp) * exp;
    output = target + (change + temp) * exp;
    if ((target - current > 0.0f) == (output > target)) {
        output = target;
        *vel = 0.0f;
    }
    return output;
}

/* The desired yaw nearest the shown one, so the spring does not spin. */
static gfloat
nearest_yaw(gfloat shown, gfloat desired)
{
    gfloat delta = desired - shown;

    while (delta > (gfloat)G_PI)
        delta -= 2.0f * (gfloat)G_PI;
    while (delta < -(gfloat)G_PI)
        delta += 2.0f * (gfloat)G_PI;
    return shown + delta;
}

static void
rest(gfloat *shown, gfloat desired, gfloat *vel, gfloat pos_eps)
{
    if (fabsf(*shown - desired) < pos_eps && fabsf(*vel) < REST_VEL) {
        *shown = desired;
        *vel = 0.0f;
    }
}

ClawtOfficeCamera *
clawt_office_camera_new(void)
{
    ClawtOfficeCamera *self = g_new0(ClawtOfficeCamera, 1);

    self->des_yaw = CLAWT_OFFICE_CAMERA_YAW_HOME;
    self->shown_yaw = CLAWT_OFFICE_CAMERA_YAW_HOME;
    self->des_pitch = clawt_office_camera_pitch_fit();
    self->shown_pitch = self->des_pitch;
    self->des_dist = 10.0f;
    self->shown_dist = 10.0f;
    return self;
}

ClawtOfficeCamera *
clawt_office_camera_copy(ClawtOfficeCamera *self)
{
    ClawtOfficeCamera *copy;

    if (self == NULL)
        return NULL;
    copy = g_new(ClawtOfficeCamera, 1);
    *copy = *self;
    return copy;
}

void
clawt_office_camera_free(ClawtOfficeCamera *self)
{
    g_free(self);
}

gfloat
clawt_office_camera_pitch_fit(void)
{
    return atanf(CLAWT_OFFICE_VIEW_3D_LIFT);
}

void
clawt_office_camera_set_bounds(ClawtOfficeCamera *self,
                               gfloat             min_x,
                               gfloat             max_x,
                               gfloat             min_z,
                               gfloat             max_z)
{
    g_return_if_fail(self != NULL);

    self->min_x = MIN(min_x, max_x);
    self->max_x = MAX(min_x, max_x);
    self->min_z = MIN(min_z, max_z);
    self->max_z = MAX(min_z, max_z);
    self->have_bounds = TRUE;
    clamp_target(self, &self->des_x, &self->des_z);
    clamp_target(self, &self->shown_x, &self->shown_z);
}

void
clawt_office_camera_set_desired_target(ClawtOfficeCamera *self, gfloat x, gfloat z)
{
    g_return_if_fail(self != NULL);

    self->des_x = x;
    self->des_z = z;
    clamp_target(self, &self->des_x, &self->des_z);
}

void
clawt_office_camera_set_desired_yaw(ClawtOfficeCamera *self, gfloat yaw)
{
    g_return_if_fail(self != NULL);

    self->des_yaw = yaw;
}

void
clawt_office_camera_set_desired_pitch(ClawtOfficeCamera *self, gfloat pitch)
{
    g_return_if_fail(self != NULL);

    self->des_pitch = clamp_pitch(pitch);
}

void
clawt_office_camera_set_desired_distance(ClawtOfficeCamera *self, gfloat distance)
{
    g_return_if_fail(self != NULL);

    self->des_dist = clamp_dist(distance);
}

void
clawt_office_camera_snap(ClawtOfficeCamera *self)
{
    g_return_if_fail(self != NULL);

    clamp_target(self, &self->des_x, &self->des_z);
    self->des_pitch = clamp_pitch(self->des_pitch);
    self->des_dist = clamp_dist(self->des_dist);
    self->shown_x = self->des_x;
    self->shown_z = self->des_z;
    self->shown_yaw = self->des_yaw;
    self->shown_pitch = self->des_pitch;
    self->shown_dist = self->des_dist;
    self->vel_x = 0.0f;
    self->vel_z = 0.0f;
    self->vel_yaw = 0.0f;
    self->vel_pitch = 0.0f;
    self->vel_dist = 0.0f;
}

void
clawt_office_camera_snap_pose(ClawtOfficeCamera *self,
                              gfloat             x,
                              gfloat             z,
                              gfloat             yaw,
                              gfloat             pitch,
                              gfloat             distance)
{
    g_return_if_fail(self != NULL);

    clawt_office_camera_set_desired_target(self, x, z);
    clawt_office_camera_set_desired_yaw(self, yaw);
    clawt_office_camera_set_desired_pitch(self, pitch);
    clawt_office_camera_set_desired_distance(self, distance);
    clawt_office_camera_snap(self);
}

void
clawt_office_camera_look(ClawtOfficeCamera *self, gfloat dx_px, gfloat dy_px)
{
    g_return_if_fail(self != NULL);

    self->des_yaw += dx_px * CLAWT_OFFICE_CAMERA_LOOK_RAD_PER_PX;
    self->des_pitch = clamp_pitch(self->des_pitch +
                                  dy_px * CLAWT_OFFICE_CAMERA_LOOK_RAD_PER_PX);
}

void
clawt_office_camera_pan(ClawtOfficeCamera *self,
                        gfloat             dx_px,
                        gfloat             dy_px,
                        gfloat             view_h,
                        gfloat             fovy)
{
    gfloat yaw;
    gfloat sin_p;
    gfloat scale;
    gfloat right_x;
    gfloat right_z;
    gfloat far_x;
    gfloat far_z;

    g_return_if_fail(self != NULL);

    /*
     * right and far match clawt_office_view_project_3d()'s floor axes
     * at this yaw: right is (sin yaw, -cos yaw), and far, where the
     * camera looks, is (-cos yaw, -sin yaw).  Dragging right moves the
     * floor with the pointer, so the aim goes the other way.
     */
    yaw = self->shown_yaw;
    sin_p = sinf(self->shown_pitch);
    if (sin_p < PAN_SIN_FLOOR)
        sin_p = PAN_SIN_FLOOR;
    if (!(view_h > 1.0f))
        view_h = 1.0f;
    scale = (2.0f * self->shown_dist * tanf(fovy * (gfloat)G_PI / 360.0f)) /
            view_h / sin_p;
    right_x = sinf(yaw);
    right_z = -cosf(yaw);
    far_x = -cosf(yaw);
    far_z = -sinf(yaw);
    self->des_x -= (right_x * dx_px + far_x * dy_px) * scale;
    self->des_z -= (right_z * dx_px + far_z * dy_px) * scale;
    clamp_target(self, &self->des_x, &self->des_z);
}

void
clawt_office_camera_move(ClawtOfficeCamera *self,
                         gfloat             forward,
                         gfloat             strafe,
                         gfloat             dt,
                         gboolean           fast)
{
    gfloat speed;
    gfloat yaw;
    gfloat look_x;
    gfloat look_z;
    gfloat right_x;
    gfloat right_z;

    g_return_if_fail(self != NULL);

    if (!(dt > 0.0f) || (forward == 0.0f && strafe == 0.0f))
        return;
    speed = CLAWT_OFFICE_CAMERA_MOVE_SPEED * dt;
    if (fast)
        speed *= CLAWT_OFFICE_CAMERA_FAST_SCALE;
    yaw = self->shown_yaw;
    look_x = -cosf(yaw);
    look_z = -sinf(yaw);
    right_x = sinf(yaw);
    right_z = -cosf(yaw);
    self->des_x += (look_x * forward + right_x * strafe) * speed;
    self->des_z += (look_z * forward + right_z * strafe) * speed;
    clamp_target(self, &self->des_x, &self->des_z);
}

void
clawt_office_camera_turn(ClawtOfficeCamera *self,
                         gfloat             dir,
                         gfloat             dt,
                         gboolean           fast)
{
    gfloat speed;

    g_return_if_fail(self != NULL);

    if (!(dt > 0.0f) || dir == 0.0f)
        return;
    speed = CLAWT_OFFICE_CAMERA_TURN_SPEED * dt;
    if (fast)
        speed *= CLAWT_OFFICE_CAMERA_FAST_SCALE;
    /* +dir is left: the old left arrow decreased the orbit. */
    self->des_yaw -= dir * speed;
}

static void
slide_target(gfloat *x, gfloat *z, gfloat anchor_x, gfloat anchor_z, gfloat ratio)
{
    *x = anchor_x + (*x - anchor_x) * ratio;
    *z = anchor_z + (*z - anchor_z) * ratio;
}

void
clawt_office_camera_dolly(ClawtOfficeCamera *self,
                          gfloat             wheel,
                          gboolean           has_anchor,
                          gfloat             anchor_x,
                          gfloat             anchor_z)
{
    gfloat old;
    gfloat next;
    gfloat ratio;

    g_return_if_fail(self != NULL);

    if (wheel == 0.0f)
        return;
    old = self->des_dist;
    if (!(old > 0.0f))
        old = CLAWT_OFFICE_CAMERA_DIST_MIN;
    next = clamp_dist(old * expf(-wheel * CLAWT_OFFICE_CAMERA_DOLLY_STEP));
    ratio = next / old;
    self->des_dist = next;
    if (!has_anchor || ratio == 1.0f)
        return;
    /*
     * Slide desired and shown by the same ratio and drop the aim's
     * velocity, or the spring eases back to where the aim was and the
     * point under the cursor walks off.
     */
    slide_target(&self->des_x, &self->des_z, anchor_x, anchor_z, ratio);
    slide_target(&self->shown_x, &self->shown_z, anchor_x, anchor_z, ratio);
    clamp_target(self, &self->des_x, &self->des_z);
    clamp_target(self, &self->shown_x, &self->shown_z);
    self->vel_x = 0.0f;
    self->vel_z = 0.0f;
}

gboolean
clawt_office_camera_step(ClawtOfficeCamera *self, gfloat dt)
{
    gfloat aim_yaw;

    g_return_val_if_fail(self != NULL, FALSE);

    if (!(dt > 0.0f))
        return FALSE;
    self->shown_x = smooth_damp(self->shown_x, self->des_x, &self->vel_x, dt);
    self->shown_z = smooth_damp(self->shown_z, self->des_z, &self->vel_z, dt);
    aim_yaw = nearest_yaw(self->shown_yaw, self->des_yaw);
    self->shown_yaw = smooth_damp(self->shown_yaw, aim_yaw, &self->vel_yaw, dt);
    self->shown_pitch = smooth_damp(self->shown_pitch, self->des_pitch,
                                    &self->vel_pitch, dt);
    self->shown_dist = smooth_damp(self->shown_dist, self->des_dist,
                                   &self->vel_dist, dt);
    self->shown_pitch = clamp_pitch(self->shown_pitch);
    self->shown_dist = clamp_dist(self->shown_dist);
    clamp_target(self, &self->shown_x, &self->shown_z);
    rest(&self->shown_x, self->des_x, &self->vel_x, REST_POS);
    rest(&self->shown_z, self->des_z, &self->vel_z, REST_POS);
    rest(&self->shown_pitch, self->des_pitch, &self->vel_pitch, REST_ANG);
    rest(&self->shown_dist, self->des_dist, &self->vel_dist, REST_POS);
    rest(&self->shown_yaw, nearest_yaw(self->shown_yaw, self->des_yaw),
         &self->vel_yaw, REST_ANG);
    return fabsf(self->shown_x - self->des_x) >= REST_POS ||
           fabsf(self->shown_z - self->des_z) >= REST_POS ||
           fabsf(self->shown_pitch - self->des_pitch) >= REST_ANG ||
           fabsf(self->shown_dist - self->des_dist) >= REST_POS ||
           fabsf(nearest_yaw(self->shown_yaw, self->des_yaw) - self->shown_yaw) >= REST_ANG;
}

void
clawt_office_camera_eye(ClawtOfficeCamera *self, gfloat *x, gfloat *y, gfloat *z)
{
    gfloat cp;
    gfloat sp;

    g_return_if_fail(self != NULL);

    cp = cosf(self->shown_pitch);
    sp = sinf(self->shown_pitch);
    if (x != NULL)
        *x = self->shown_x + cosf(self->shown_yaw) * cp * self->shown_dist;
    if (y != NULL)
        *y = sp * self->shown_dist;
    if (z != NULL)
        *z = self->shown_z + sinf(self->shown_yaw) * cp * self->shown_dist;
}

void
clawt_office_camera_target(ClawtOfficeCamera *self, gfloat *x, gfloat *z)
{
    g_return_if_fail(self != NULL);

    if (x != NULL)
        *x = self->shown_x;
    if (z != NULL)
        *z = self->shown_z;
}

gfloat
clawt_office_camera_get_yaw(ClawtOfficeCamera *self)
{
    g_return_val_if_fail(self != NULL, 0.0f);

    return self->shown_yaw;
}

gfloat
clawt_office_camera_get_pitch(ClawtOfficeCamera *self)
{
    g_return_val_if_fail(self != NULL, 0.0f);

    return self->shown_pitch;
}

gfloat
clawt_office_camera_get_distance(ClawtOfficeCamera *self)
{
    g_return_val_if_fail(self != NULL, 0.0f);

    return self->shown_dist;
}

gfloat
clawt_office_camera_get_desired_yaw(ClawtOfficeCamera *self)
{
    g_return_val_if_fail(self != NULL, 0.0f);

    return self->des_yaw;
}

gfloat
clawt_office_camera_get_desired_pitch(ClawtOfficeCamera *self)
{
    g_return_val_if_fail(self != NULL, 0.0f);

    return self->des_pitch;
}

gfloat
clawt_office_camera_get_desired_distance(ClawtOfficeCamera *self)
{
    g_return_val_if_fail(self != NULL, 0.0f);

    return self->des_dist;
}

void
clawt_office_camera_get_desired_target(ClawtOfficeCamera *self, gfloat *x, gfloat *z)
{
    g_return_if_fail(self != NULL);

    if (x != NULL)
        *x = self->des_x;
    if (z != NULL)
        *z = self->des_z;
}

/*
 * The same basis as clawt_office_view_project_3d(), so a point projected
 * and then unprojected lands back on itself.
 */
static gboolean
view_basis(ClawtOfficeCamera *self, gdouble f[3], gdouble r[3], gdouble u[3])
{
    gfloat eye[3];
    gfloat target[3];
    gdouble length;
    gint i;

    clawt_office_camera_eye(self, &eye[0], &eye[1], &eye[2]);
    target[0] = self->shown_x;
    target[1] = 0.0f;
    target[2] = self->shown_z;
    for (i = 0; i < 3; i++)
        f[i] = (gdouble)target[i] - (gdouble)eye[i];
    length = sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (length <= 0.0)
        return FALSE;
    for (i = 0; i < 3; i++)
        f[i] /= length;
    r[0] = -f[2];
    r[1] = 0.0;
    r[2] = f[0];
    length = sqrt(r[0] * r[0] + r[2] * r[2]);
    if (length <= 0.0)
        return FALSE;
    r[0] /= length;
    r[2] /= length;
    u[0] = r[1] * f[2] - r[2] * f[1];
    u[1] = r[2] * f[0] - r[0] * f[2];
    u[2] = r[0] * f[1] - r[1] * f[0];
    return TRUE;
}

gboolean
clawt_office_camera_floor_at(ClawtOfficeCamera *self,
                             gfloat             px,
                             gfloat             py,
                             gfloat             view_w,
                             gfloat             view_h,
                             gfloat             fovy,
                             gfloat            *x,
                             gfloat            *z)
{
    gdouble f[3];
    gdouble r[3];
    gdouble u[3];
    gdouble dir[3];
    gdouble ndc_x;
    gdouble ndc_y;
    gdouble t;
    gdouble aspect;
    gdouble k;
    gfloat eye[3];
    gint i;

    g_return_val_if_fail(self != NULL, FALSE);

    if (!(view_w > 1.0f) || !(view_h > 1.0f))
        return FALSE;
    if (!view_basis(self, f, r, u))
        return FALSE;
    ndc_x = ((gdouble)px / (gdouble)view_w) * 2.0 - 1.0;
    ndc_y = 1.0 - ((gdouble)py / (gdouble)view_h) * 2.0;
    t = tan((gdouble)fovy * G_PI / 360.0);
    aspect = (gdouble)view_w / (gdouble)view_h;
    for (i = 0; i < 3; i++)
        dir[i] = r[i] * (ndc_x * t * aspect) + u[i] * (ndc_y * t) + f[i];
    if (!(dir[1] < -1.0e-8))
        return FALSE;
    clawt_office_camera_eye(self, &eye[0], &eye[1], &eye[2]);
    k = -(gdouble)eye[1] / dir[1];
    if (!(k > 0.0))
        return FALSE;
    if (x != NULL)
        *x = (gfloat)((gdouble)eye[0] + dir[0] * k);
    if (z != NULL)
        *z = (gfloat)((gdouble)eye[2] + dir[2] * k);
    return TRUE;
}

void
clawt_office_camera_zoom_around(gfloat *centre_x,
                                gfloat *centre_y,
                                gfloat  old_zoom,
                                gfloat  new_zoom,
                                gfloat  cursor_x,
                                gfloat  cursor_y,
                                gfloat  view_w,
                                gfloat  view_h)
{
    gfloat world_x;
    gfloat world_y;
    gfloat ox;
    gfloat oy;

    g_return_if_fail(centre_x != NULL && centre_y != NULL);

    if (!(old_zoom > 0.0f) || !(new_zoom > 0.0f))
        return;
    ox = cursor_x - view_w * 0.5f;
    oy = cursor_y - view_h * 0.5f;
    world_x = *centre_x + ox / old_zoom;
    world_y = *centre_y + oy / old_zoom;
    *centre_x = world_x - ox / new_zoom;
    *centre_y = world_y - oy / new_zoom;
}

gfloat
clawt_office_camera_zoom_factor(gfloat zoom, gfloat wheel)
{
    if (!(zoom > 0.0f))
        zoom = 1.0f;
    return CLAMP(zoom * expf(wheel * CLAWT_OFFICE_CAMERA_ZOOM_STEP),
                 CLAWT_OFFICE_CAMERA_ZOOM_MIN, CLAWT_OFFICE_CAMERA_ZOOM_MAX);
}

gboolean
clawt_office_camera_double_click(gfloat since, gfloat dx, gfloat dy)
{
    gfloat reach = CLAWT_OFFICE_CAMERA_DOUBLE_CLICK_PX;

    if (!(since >= 0.0f) || since > CLAWT_OFFICE_CAMERA_DOUBLE_CLICK_S)
        return FALSE;
    return (dx * dx + dy * dy) <= (reach * reach);
}

ClawtOfficeBindAction
clawt_office_binding_action(guint key)
{
    guint i;

    for (i = 0; i < G_N_ELEMENTS(bindings); i++) {
        if (bindings[i].key == key)
            return bindings[i].action;
    }
    return CLAWT_OFFICE_BIND_NONE;
}

guint
clawt_office_binding_count(void)
{
    return G_N_ELEMENTS(bindings);
}

guint
clawt_office_binding_key(guint index)
{
    if (index >= G_N_ELEMENTS(bindings))
        return 0;
    return bindings[index].key;
}

static guint
letter_key(guint keyval, guint lower, guint ray)
{
    if (keyval == lower || keyval == (lower - 32u))
        return ray;
    return 0;
}

guint
clawt_office_key_from_gdk(guint keyval)
{
    guint ray;

    ray = letter_key(keyval, GDK_W, CLAWT_OFFICE_KEY_W);
    if (ray != 0)
        return ray;
    ray = letter_key(keyval, GDK_A, CLAWT_OFFICE_KEY_A);
    if (ray != 0)
        return ray;
    ray = letter_key(keyval, GDK_S, CLAWT_OFFICE_KEY_S);
    if (ray != 0)
        return ray;
    ray = letter_key(keyval, GDK_D, CLAWT_OFFICE_KEY_D);
    if (ray != 0)
        return ray;
    ray = letter_key(keyval, GDK_Q, CLAWT_OFFICE_KEY_Q);
    if (ray != 0)
        return ray;
    ray = letter_key(keyval, GDK_E, CLAWT_OFFICE_KEY_E);
    if (ray != 0)
        return ray;
    ray = letter_key(keyval, GDK_F, CLAWT_OFFICE_KEY_F);
    if (ray != 0)
        return ray;
    switch (keyval) {
    case GDK_ESCAPE:  return CLAWT_OFFICE_KEY_ESCAPE;
    case GDK_HOME:    return CLAWT_OFFICE_KEY_HOME;
    case GDK_LEFT:    return CLAWT_OFFICE_KEY_LEFT;
    case GDK_UP:      return CLAWT_OFFICE_KEY_UP;
    case GDK_RIGHT:   return CLAWT_OFFICE_KEY_RIGHT;
    case GDK_DOWN:    return CLAWT_OFFICE_KEY_DOWN;
    case GDK_SHIFT_L: return CLAWT_OFFICE_KEY_LEFT_SHIFT;
    case GDK_SHIFT_R: return CLAWT_OFFICE_KEY_RIGHT_SHIFT;
    default:          return 0;
    }
}

guint
clawt_office_modifiers_from_gdk(guint state)
{
    guint mods = 0;

    if (state & GDK_SHIFT_BIT)
        mods |= 1u;
    if (state & GDK_CONTROL_BIT)
        mods |= 2u;
    if (state & GDK_ALT_BIT)
        mods |= 4u;
    if (state & GDK_SUPER_BIT)
        mods |= 8u;
    return mods;
}

void
clawt_office_input_note_key(ClawtOfficeInput *input,
                            guint             key,
                            gboolean          pressed,
                            gboolean          down)
{
    g_return_if_fail(input != NULL);

    switch (clawt_office_binding_action(key)) {
    case CLAWT_OFFICE_BIND_FORWARD:
        if (down)
            input->move_forward = TRUE;
        break;
    case CLAWT_OFFICE_BIND_BACK:
        if (down)
            input->move_back = TRUE;
        break;
    case CLAWT_OFFICE_BIND_LEFT:
        if (down)
            input->move_left = TRUE;
        break;
    case CLAWT_OFFICE_BIND_RIGHT:
        if (down)
            input->move_right = TRUE;
        break;
    case CLAWT_OFFICE_BIND_TURN_LEFT:
        if (down)
            input->turn_left = TRUE;
        break;
    case CLAWT_OFFICE_BIND_TURN_RIGHT:
        if (down)
            input->turn_right = TRUE;
        break;
    case CLAWT_OFFICE_BIND_FASTER:
        if (down)
            input->shift = TRUE;
        break;
    case CLAWT_OFFICE_BIND_FIT:
        if (pressed)
            input->reset_fit = TRUE;
        break;
    case CLAWT_OFFICE_BIND_NONE:
    default:
        break;
    }
}
