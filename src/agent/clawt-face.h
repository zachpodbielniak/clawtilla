/*
 * clawt-face.h - The generated face every agent without a picture wears
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A face is a coloured blob with two rounded eyes.  The blob's outline
 * and the eyes' geometry live here, in plain C with no toolkit, so the
 * GTK, lrg and web clients -- and the CLI's SVG export -- all draw the
 * same face from the same numbers.  The clients only decide *how* to
 * paint: a GskPath, a triangle fan, an inline SVG.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>

G_BEGIN_DECLS

/**
 * CLAWT_FACE_INITIALS:
 *
 * What clawt_face_resolve() answers for `agents.face: initials` -- the
 * letter disc clawtilla drew before faces existed.  Never a valid shape
 * index, so a caller cannot mistake it for one.
 */
#define CLAWT_FACE_INITIALS (-1)

/**
 * CLAWT_FACE_INITIALS_NAME:
 *
 * The configured value that asks for the letter disc.
 */
#define CLAWT_FACE_INITIALS_NAME "initials"

/**
 * CLAWT_FACE_BLINK_MS:
 *
 * How long one blink takes, closed and open again, in milliseconds.
 */
#define CLAWT_FACE_BLINK_MS (160)

/**
 * CLAWT_FACE_BLINK_GAP_MIN_MS:
 *
 * The shortest time between the starts of two separate blinks.
 */
#define CLAWT_FACE_BLINK_GAP_MIN_MS (2500)

/**
 * CLAWT_FACE_BLINK_GAP_MAX_MS:
 *
 * The longest time between the starts of two separate blinks.
 */
#define CLAWT_FACE_BLINK_GAP_MAX_MS (6000)

/**
 * CLAWT_FACE_LOOK_REACH:
 *
 * How far, in unit-box coordinates, the eyes move sideways when looking
 * as far as they can.  Up and down move 70% of this: a face looking up
 * shifts less than one looking aside, as eyes in a round face do.
 */
#define CLAWT_FACE_LOOK_REACH (0.2)

/**
 * CLAWT_FACE_PALETTE_SIZE:
 *
 * How many colours clawt_face_palette_color() offers a picker.
 */
#define CLAWT_FACE_PALETTE_SIZE (12)

/**
 * ClawtFaceEye:
 * @x: the eye's centre, across, in the unit box (-1 left .. 1 right)
 * @y: the eye's centre, down, in the unit box (-1 top .. 1 bottom)
 * @width: its width
 * @height: its height; shrinks as the face blinks
 * @radius: its corner radius, never more than half the smaller side
 *
 * One eye: a rounded rectangle, upright.
 */
typedef struct {
    gdouble x;
    gdouble y;
    gdouble width;
    gdouble height;
    gdouble radius;
} ClawtFaceEye;

/**
 * ClawtFaceEyes:
 * @left: the eye on the viewer's left
 * @right: the eye on the viewer's right
 *
 * Both eyes of a face in one pose.
 */
typedef struct {
    ClawtFaceEye left;
    ClawtFaceEye right;
} ClawtFaceEyes;

/**
 * ClawtFacePose:
 * @look_x: where the eyes look, across: -1 (left) .. 1 (right)
 * @look_y: where the eyes look, down: -1 (up) .. 1 (down)
 * @blink: how closed the eyes are: 0 (open) .. 1 (shut)
 *
 * A moment of a face: what it is looking at and whether it is blinking.
 * A zeroed pose is a face at rest, looking straight out.
 */
typedef struct {
    gdouble look_x;
    gdouble look_y;
    gdouble blink;
} ClawtFacePose;

/**
 * clawt_face_shape_count:
 *
 * How many shapes there are.  Indices run from 0 to one below this.
 *
 * Returns: the number of shapes
 */
guint clawt_face_shape_count(void);

/**
 * clawt_face_shape_name:
 * @shape: a shape index
 *
 * The name `agents.face` takes for @shape: lowercase, one word.
 *
 * Returns: (transfer none) (nullable): the name, or %NULL if @shape is
 *   out of range
 */
const gchar *clawt_face_shape_name(gint shape);

/**
 * clawt_face_shape_label:
 * @shape: a shape index
 *
 * The name to show a person for @shape, capitalised.
 *
 * Returns: (transfer none) (nullable): the label, or %NULL if @shape is
 *   out of range
 */
const gchar *clawt_face_shape_label(gint shape);

/**
 * clawt_face_shape_lookup:
 * @name: (nullable): a configured `agents.face` value
 *
 * The shape @name names.  Matching ignores case and surrounding space,
 * since it is typed into YAML by hand.
 *
 * Returns: the shape index; %CLAWT_FACE_INITIALS for `initials`; or -2
 *   if @name names nothing (including %NULL and the empty string)
 */
gint clawt_face_shape_lookup(const gchar *name);

/**
 * clawt_face_value_is_valid:
 * @value: (nullable): a value someone wants to store in `agents.face`
 *
 * Whether @value may be written to `agents.face`: a shape name,
 * `initials`, or empty (which means "pick one for me").
 *
 * Returns: %TRUE if it may
 */
gboolean clawt_face_value_is_valid(const gchar *value);

/**
 * clawt_face_valid_values:
 *
 * Every value clawt_face_value_is_valid() accepts apart from empty,
 * comma-separated, for an error message that tells the person what to
 * type instead.
 *
 * Returns: (transfer full): the list
 */
gchar *clawt_face_valid_values(void);

/**
 * clawt_face_seed:
 * @agent_id: (nullable): the agent's id
 *
 * A number that is the same for an agent in every process and every
 * client, and different (as far as a hash can manage) from one agent to
 * the next.  It picks the automatic shape and spreads blinks apart.
 *
 * Returns: the seed
 */
guint32 clawt_face_seed(const gchar *agent_id);

/**
 * clawt_face_resolve:
 * @agent_id: (nullable): the agent's id
 * @configured: (nullable): its `agents.face`
 *
 * Which face the agent wears.
 *
 * A configured shape name wins; `initials` keeps the letter disc; an
 * empty or unknown value picks a shape from clawt_face_seed() -- an
 * unknown one is a typo, and a typo in a cosmetic setting should cost
 * the agent its chosen face, never its face altogether.
 *
 * Returns: a shape index, or %CLAWT_FACE_INITIALS
 */
gint clawt_face_resolve(const gchar *agent_id, const gchar *configured);

/**
 * clawt_face_resolve_name:
 * @agent_id: (nullable): the agent's id
 * @configured: (nullable): its `agents.face`
 *
 * clawt_face_resolve(), as the name a client is sent.
 *
 * Returns: (transfer none): a shape name, or `initials`
 */
const gchar *clawt_face_resolve_name(const gchar *agent_id,
                                     const gchar *configured);

/**
 * clawt_face_outline:
 * @shape: a shape index
 * @n_points: how many points to sample, at least 3
 * @xy: (out caller-allocates) (array): 2 × @n_points doubles, x then y
 *   for each point
 *
 * The blob's outline, clockwise on screen from the right-hand side,
 * in the unit box: x and y both run -1 .. 1, y down.
 *
 * Every outline is star-shaped around (0, 0): a straight line from the
 * centre meets the edge exactly once.  That is what lets lrg fill it as
 * a triangle fan and the tests check it with a ray cast.
 *
 * An out-of-range @shape is drawn as shape 0.
 */
void clawt_face_outline(gint shape, guint n_points, gdouble *xy);

/**
 * clawt_face_radius_at:
 * @shape: a shape index
 * @theta: an angle in radians, 0 pointing right, growing clockwise on
 *   screen
 *
 * How far the edge is from the centre in direction @theta, in the unit
 * box.  clawt_face_outline() samples this.
 *
 * Returns: the radius, above 0 and at most 1
 */
gdouble clawt_face_radius_at(gint shape, gdouble theta);

/**
 * clawt_face_eyes:
 * @shape: a shape index
 * @pose: (nullable): what the face is doing; %NULL for at rest
 * @out: (out caller-allocates): the eyes
 *
 * Where the eyes are, and how open, for @shape in @pose.
 *
 * The look is clamped to the unit disc before it moves the eyes, so a
 * pointer far away gives the same look as one just off the edge, and a
 * diagonal look never reaches further than a straight one.
 */
void clawt_face_eyes(gint shape, const ClawtFacePose *pose,
                     ClawtFaceEyes *out);

/**
 * clawt_face_look_at:
 * @center_x: the face's centre, in any coordinates
 * @center_y: the face's centre, in the same coordinates
 * @radius: the face's radius, in the same units
 * @pointer_x: the pointer, in the same coordinates
 * @pointer_y: the pointer, in the same coordinates
 * @look_x: (out): the look, across
 * @look_y: (out): the look, down
 *
 * Which way a face should look to watch the pointer.
 *
 * The look grows with distance and reaches its limit at three radii,
 * so a pointer moving over the face itself turns the eyes gently and
 * one across the row turns them all the way.  A pointer exactly on the
 * centre (or a @radius of 0) gives (0, 0).
 */
void clawt_face_look_at(gdouble  center_x,
                        gdouble  center_y,
                        gdouble  radius,
                        gdouble  pointer_x,
                        gdouble  pointer_y,
                        gdouble *look_x,
                        gdouble *look_y);

/**
 * clawt_face_ease:
 * @current: where the look is now
 * @target: where it is heading
 * @dt_ms: milliseconds since the last frame
 *
 * One frame of the look easing toward its target: an exponential
 * approach with a time constant of about 70 ms, which is quick enough
 * to feel attentive and slow enough not to jitter.  Snaps to @target
 * once within 0.002, so a caller can stop animating when it equals.
 *
 * Returns: the new value
 */
gdouble clawt_face_ease(gdouble current, gdouble target, gdouble dt_ms);

/**
 * clawt_face_blink_period:
 * @seed: clawt_face_seed() of the agent
 *
 * The agent's own average time between blinks, in milliseconds.
 *
 * Returns: the period
 */
guint clawt_face_blink_period(guint32 seed);

/**
 * clawt_face_blink_at:
 * @seed: clawt_face_seed() of the agent
 * @t_ms: a time in milliseconds, from any fixed origin
 *
 * How closed the agent's eyes are at @t_ms: 0 open, 1 shut.
 *
 * A pure function of its arguments, so every client blinks an agent
 * the same way without keeping any state, and a test can check the
 * schedule.  Blinks start between %CLAWT_FACE_BLINK_GAP_MIN_MS and
 * %CLAWT_FACE_BLINK_GAP_MAX_MS apart, jittered per blink; about one
 * in five is a double blink.
 *
 * Returns: the blink amount
 */
gdouble clawt_face_blink_at(guint32 seed, gint64 t_ms);

/**
 * clawt_face_next_blink:
 * @seed: clawt_face_seed() of the agent
 * @t_ms: now, in the same time base as clawt_face_blink_at()
 *
 * When the next blink starts, at or after @t_ms -- so a client can arm
 * one timer and sleep until then, rather than redrawing every frame to
 * find out.
 *
 * Returns: the start of the next blink, in milliseconds
 */
gint64 clawt_face_next_blink(guint32 seed, gint64 t_ms);

/**
 * clawt_face_eye_color:
 * @fill: (nullable): the face's colour, `#rgb` or `#rrggbb`
 *
 * The eyes' colour on @fill: near-black, or near-white when @fill is
 * dark enough that white eyes have the better contrast.
 *
 * Returns: (transfer none): the colour, as `#rrggbb`
 */
const gchar *clawt_face_eye_color(const gchar *fill);

/**
 * clawt_face_palette_color:
 * @index: below %CLAWT_FACE_PALETTE_SIZE
 *
 * One of the colours a face picker offers as swatches.  Brighter than
 * the derived avatar tones, which were picked for white initials rather
 * than for a body with eyes on it.
 *
 * Returns: (transfer none) (nullable): `#rrggbb`, or %NULL if out of range
 */
const gchar *clawt_face_palette_color(guint index);

/**
 * clawt_face_svg:
 * @shape: a shape index
 * @fill: the face's colour; must pass clawt_color_ink()
 * @size: the width and height to declare, in pixels
 * @pose: (nullable): the pose to draw; %NULL for at rest
 * @seed: clawt_face_seed() of the agent, used when @animated
 * @animated: whether to emit the class names and per-face CSS variables
 *   the web client's stylesheet animates (blink and look)
 *
 * The face as a self-contained SVG element.
 *
 * @fill is refused unless clawt_color_ink() accepts it -- the colour is
 * written into markup, and a typed colour such as `red"/><script>` must
 * never reach it.  Every number is written with g_ascii_formatd(), so a
 * locale with a decimal comma cannot break the path.
 *
 * Returns: (transfer full) (nullable): the `<svg>` markup, or %NULL if
 *   @fill is not a colour
 */
gchar *clawt_face_svg(gint                 shape,
                      const gchar         *fill,
                      guint                size,
                      const ClawtFacePose *pose,
                      guint32              seed,
                      gboolean             animated);

G_END_DECLS
