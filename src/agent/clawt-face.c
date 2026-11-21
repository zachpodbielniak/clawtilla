/*
 * clawt-face.c - The generated face every agent without a picture wears
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Every shape is a polar radius function
 *
 *     r(θ) = super(θ - rotate) × (1 + Σ amp_k · cos(k·(θ - rotate) + phase_k))
 *
 * where super() is a superellipse (1 for a plain circle).  A polar
 * function is star-shaped around its origin by construction, which is
 * the one property every client relies on: lrg fills it as a triangle
 * fan from the centre, GTK and SVG draw it as one closed path, and the
 * tests can ray-cast it.  Each shape is then scaled so its furthest
 * point sits at FACE_FILL of the unit box, leaving room for a glow or
 * a selection ring around it.
 */

#include "clawtilla.h"
#include "agent/clawt-face.h"
#include "clawt-util.h"

#include <math.h>
#include <string.h>

/* defines */
#define FACE_FILL          (0.92)  /* furthest edge, as a share of the box */
#define FACE_NORM_SAMPLES  (720)   /* samples taken to find that edge */
#define FACE_EYE_WIDTH     (0.17)
#define FACE_EYE_HEIGHT    (0.32)
#define FACE_EYE_MIN       (0.035) /* a shut eye is a line this thick */
#define FACE_LOOK_REACH_Y  (0.7)   /* share of the reach up and down */
#define FACE_LOOK_SPAN     (3.0)   /* radii to reach full look */
#define FACE_EASE_MS       (70.0)
#define FACE_DOUBLE_GAP_MS (260)   /* second blink of a double, after the first */
#define FACE_BLINK_BASE_MS (3300)
#define FACE_BLINK_SPREAD  (1700)  /* period is BASE .. BASE + SPREAD - 1 */
#define FACE_BLINK_JITTER  (800)   /* each blink lands this much late, at most */
#define FACE_SEED_SALT     (0x5f3759dfU)

typedef struct {
    guint   k;
    gdouble amp;
    gdouble phase;
} FaceHarmonic;

typedef struct {
    const gchar  *name;
    const gchar  *label;
    gdouble       super;   /* superellipse exponent; 0 for a circle */
    gdouble       rotate;  /* radians, clockwise on screen */
    FaceHarmonic  h[3];    /* unused ones have k == 0 */
    gdouble       eye_x;   /* centre between the eyes, across */
    gdouble       eye_y;   /* and down */
    gdouble       eye_gap; /* centre of each eye from that point */
} FaceShape;

/*
 * The shapes.  Order is part of the format: clawt_face_resolve() picks
 * an automatic face by index, so reordering this table would give every
 * agent without a configured face a different one.  Append only.
 */
static const FaceShape face_shapes[] = {
    { "pebble",   "Pebble",   0.0, 0.0,
      { { 2, 0.05, 0.0 }, { 0, 0, 0 }, { 0, 0, 0 } },
      0.0, -0.06, 0.21 },
    { "cloud",    "Cloud",    0.0, 0.0,
      { { 6, 0.06, G_PI / 2.0 }, { 2, 0.07, 0.0 }, { 0, 0, 0 } },
      0.0, -0.02, 0.21 },
    { "cushion",  "Cushion",  3.2, 0.14,
      { { 4, 0.03, 0.0 }, { 0, 0, 0 }, { 0, 0, 0 } },
      0.0, -0.05, 0.22 },
    { "clover",   "Clover",   0.0, 0.0,
      { { 4, 0.15, G_PI }, { 0, 0, 0 }, { 0, 0, 0 } },
      0.0, -0.04, 0.18 },
    { "star",     "Star",     0.0, 0.0,
      { { 4, 0.22, 0.0 }, { 0, 0, 0 }, { 0, 0, 0 } },
      0.0, -0.03, 0.15 },
    { "bean",     "Bean",     0.0, 0.0,
      { { 1, 0.07, G_PI }, { 2, 0.12, 0.4 }, { 3, 0.03, 0.0 } },
      0.04, -0.06, 0.19 },
    { "flower",   "Flower",   0.0, 0.0,
      { { 6, 0.09, G_PI / 2.0 }, { 0, 0, 0 }, { 0, 0, 0 } },
      0.0, -0.03, 0.18 },
    { "drop",     "Drop",     0.0, 0.0,
      { { 1, 0.22, G_PI / 2.0 }, { 2, 0.06, 0.0 }, { 0, 0, 0 } },
      0.0, 0.06, 0.19 },
    { "gem",      "Gem",      2.6, 0.35,
      { { 1, 0.03, 0.0 }, { 0, 0, 0 }, { 0, 0, 0 } },
      0.0, -0.04, 0.19 },
    { "petal",    "Petal",    0.0, 0.0,
      { { 3, 0.12, G_PI / 2.0 }, { 0, 0, 0 }, { 0, 0, 0 } },
      0.0, 0.0, 0.18 },
    { "puff",     "Puff",     0.0, 0.0,
      { { 5, 0.05, 0.0 }, { 3, 0.07, 0.6 }, { 1, 0.03, 0.0 } },
      0.0, -0.04, 0.19 },
    { "squircle", "Squircle", 5.0, 0.0,
      { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } },
      0.0, -0.05, 0.23 },
    { "wedge",    "Wedge",    0.0, 0.5,
      { { 3, 0.17, 0.0 }, { 0, 0, 0 }, { 0, 0, 0 } },
      -0.08, 0.0, 0.16 },
    { "bloom",    "Bloom",    0.0, 0.0,
      { { 5, 0.13, G_PI / 2.0 }, { 0, 0, 0 }, { 0, 0, 0 } },
      0.0, 0.0, 0.16 }
};

#define FACE_SHAPE_COUNT (G_N_ELEMENTS(face_shapes))

/*
 * Brighter than the avatar tones: those were chosen for white initials,
 * these for a body with dark eyes on it.  A picker shows them in order.
 */
static const gchar *const face_palette[CLAWT_FACE_PALETTE_SIZE] = {
    "#f4f4f5",  /* white */
    "#a0703f",  /* brown */
    "#e5484d",  /* red */
    "#f2711c",  /* orange */
    "#f2a516",  /* amber */
    "#22b573",  /* green */
    "#14a8a0",  /* teal */
    "#2f7de1",  /* blue */
    "#5b6ef5",  /* indigo */
    "#8e5cf0",  /* violet */
    "#e8459a",  /* pink */
    "#7d7f86"   /* gray */
};

/* ── Shapes ───────────────────────────────────────────────────────── */

guint
clawt_face_shape_count(void)
{
    return FACE_SHAPE_COUNT;
}

/* An index, clamped into the table: drawing never fails on a bad one. */
static const FaceShape *
shape_of(gint shape)
{
    if (shape < 0 || (guint)shape >= FACE_SHAPE_COUNT)
        return &face_shapes[0];

    return &face_shapes[shape];
}

const gchar *
clawt_face_shape_name(gint shape)
{
    if (shape < 0 || (guint)shape >= FACE_SHAPE_COUNT)
        return NULL;

    return face_shapes[shape].name;
}

const gchar *
clawt_face_shape_label(gint shape)
{
    if (shape < 0 || (guint)shape >= FACE_SHAPE_COUNT)
        return NULL;

    return face_shapes[shape].label;
}

gint
clawt_face_shape_lookup(const gchar *name)
{
    g_autofree gchar *clean = NULL;
    guint i;

    if (name == NULL)
        return -2;

    /* Typed into YAML by hand: case and stray spaces are not a new name. */
    clean = g_ascii_strdown(name, -1);
    g_strstrip(clean);

    if (*clean == '\0')
        return -2;

    if (g_strcmp0(clean, CLAWT_FACE_INITIALS_NAME) == 0)
        return CLAWT_FACE_INITIALS;

    for (i = 0; i < FACE_SHAPE_COUNT; i++) {
        if (g_strcmp0(clean, face_shapes[i].name) == 0)
            return (gint)i;
    }

    return -2;
}

gboolean
clawt_face_value_is_valid(const gchar *value)
{
    g_autofree gchar *clean = NULL;

    if (value == NULL)
        return TRUE;

    clean = g_strstrip(g_strdup(value));

    /* Empty is "choose for me", which is always allowed. */
    if (*clean == '\0')
        return TRUE;

    return clawt_face_shape_lookup(clean) != -2;
}

gchar *
clawt_face_valid_values(void)
{
    GString *list = g_string_new(NULL);
    guint i;

    for (i = 0; i < FACE_SHAPE_COUNT; i++) {
        g_string_append(list, face_shapes[i].name);
        g_string_append(list, ", ");
    }
    g_string_append(list, CLAWT_FACE_INITIALS_NAME);

    return g_string_free(list, FALSE);
}

/* A 32-bit finaliser: every input bit reaches every output bit. */
static guint32
mix32(guint32 h)
{
    h ^= h >> 16;
    h *= 0x45d9f3bU;
    h ^= h >> 16;
    h *= 0x45d9f3bU;
    h ^= h >> 16;

    return h;
}

guint32
clawt_face_seed(const gchar *agent_id)
{
    /*
     * g_str_hash() is djb2, the same in every process -- the property
     * that matters, since a face drawn by GTK and by the web client must
     * be the same face.  The salt keeps the shape independent of the
     * avatar tone, which hashes the same id without it: otherwise every
     * blue agent would be a cloud.
     */
    if (agent_id == NULL || *agent_id == '\0')
        return mix32(FACE_SEED_SALT);

    return mix32(g_str_hash(agent_id) ^ FACE_SEED_SALT);
}

gint
clawt_face_resolve(const gchar *agent_id, const gchar *configured)
{
    gint named = clawt_face_shape_lookup(configured);

    if (named != -2)
        return named;

    return (gint)(clawt_face_seed(agent_id) % FACE_SHAPE_COUNT);
}

const gchar *
clawt_face_resolve_name(const gchar *agent_id, const gchar *configured)
{
    gint shape = clawt_face_resolve(agent_id, configured);

    if (shape == CLAWT_FACE_INITIALS)
        return CLAWT_FACE_INITIALS_NAME;

    return face_shapes[shape].name;
}

/* ── Outline ──────────────────────────────────────────────────────── */

/*
 * The radius before scaling.  The superellipse is |cos|^p + |sin|^p = r^-p
 * solved for r; the harmonics then bulge and pinch it.  The amplitudes
 * in the table add up to well under 1, so the bracket never reaches 0
 * and the shape never folds through its centre.
 */
static gdouble
raw_radius(const FaceShape *s, gdouble theta)
{
    gdouble t = theta - s->rotate;
    gdouble r = 1.0;
    gdouble bulge = 1.0;
    guint i;

    if (s->super > 0.0) {
        gdouble c = pow(fabs(cos(t)), s->super);
        gdouble n = pow(fabs(sin(t)), s->super);

        r = pow(c + n, -1.0 / s->super);
    }

    for (i = 0; i < G_N_ELEMENTS(s->h); i++) {
        if (s->h[i].k == 0)
            continue;
        bulge += s->h[i].amp * cos((gdouble)s->h[i].k * t + s->h[i].phase);
    }

    return r * bulge;
}

/*
 * Each shape's furthest raw radius, found once by sampling.  The table
 * is constant, so this is computed the first time any face is drawn and
 * never again; g_once makes that safe from any thread.
 */
static const gdouble *
shape_scales(void)
{
    static gdouble scales[G_N_ELEMENTS(face_shapes)];
    static gsize ready = 0;

    if (g_once_init_enter(&ready)) {
        guint i;
        guint j;

        for (i = 0; i < FACE_SHAPE_COUNT; i++) {
            gdouble most = 0.0;

            for (j = 0; j < FACE_NORM_SAMPLES; j++) {
                gdouble theta = 2.0 * G_PI * j / FACE_NORM_SAMPLES;
                gdouble r = raw_radius(&face_shapes[i], theta);

                if (r > most)
                    most = r;
            }
            scales[i] = FACE_FILL / most;
        }
        g_once_init_leave(&ready, 1);
    }

    return scales;
}

gdouble
clawt_face_radius_at(gint shape, gdouble theta)
{
    const FaceShape *s = shape_of(shape);
    const gdouble *scales = shape_scales();

    return raw_radius(s, theta) * scales[s - face_shapes];
}

void
clawt_face_outline(gint shape, guint n_points, gdouble *xy)
{
    guint i;

    g_return_if_fail(xy != NULL);
    g_return_if_fail(n_points >= 3);

    for (i = 0; i < n_points; i++) {
        gdouble theta = 2.0 * G_PI * i / n_points;
        gdouble r = clawt_face_radius_at(shape, theta);

        xy[i * 2] = r * cos(theta);
        xy[i * 2 + 1] = r * sin(theta);
    }
}

void
clawt_face_coverage(gint shape, guint size, guint8 *alpha)
{
    enum { SAMPLES = 4 };
    gdouble half;
    guint px;
    guint py;

    g_return_if_fail(alpha != NULL);
    g_return_if_fail(size > 0);

    half = size / 2.0;

    /*
     * Each pixel is SAMPLES × SAMPLES points, each inside when it is
     * nearer the centre than the edge is in its direction -- the same
     * star-shaped test the tests use, so the mask is the outline.
     */
    for (py = 0; py < size; py++) {
        for (px = 0; px < size; px++) {
            guint inside = 0;
            guint sx;
            guint sy;

            for (sy = 0; sy < SAMPLES; sy++) {
                for (sx = 0; sx < SAMPLES; sx++) {
                    gdouble x = (px + (sx + 0.5) / SAMPLES - half) / half;
                    gdouble y = (py + (sy + 0.5) / SAMPLES - half) / half;
                    gdouble r = sqrt(x * x + y * y);

                    if (r <= clawt_face_radius_at(shape, atan2(y, x)))
                        inside++;
                }
            }

            alpha[py * size + px] =
                (guint8)((inside * 255 + SAMPLES * SAMPLES / 2) /
                         (SAMPLES * SAMPLES));
        }
    }
}

/* ── Eyes ─────────────────────────────────────────────────────────── */

void
clawt_face_eyes(gint shape, const ClawtFacePose *pose, ClawtFaceEyes *out)
{
    const FaceShape *s = shape_of(shape);
    gdouble look_x = 0.0;
    gdouble look_y = 0.0;
    gdouble blink = 0.0;
    gdouble height;
    gdouble length;
    gdouble cx;
    gdouble cy;

    g_return_if_fail(out != NULL);

    if (pose != NULL) {
        look_x = isfinite(pose->look_x) ? pose->look_x : 0.0;
        look_y = isfinite(pose->look_y) ? pose->look_y : 0.0;
        blink = isfinite(pose->blink) ? CLAMP(pose->blink, 0.0, 1.0) : 0.0;
    }

    /*
     * Into the unit disc, so a diagonal look reaches no further than a
     * straight one -- a square clamp would push the eyes into a corner
     * of the face that a narrow shape does not have.
     */
    length = sqrt(look_x * look_x + look_y * look_y);
    if (length > 1.0) {
        look_x /= length;
        look_y /= length;
    }

    cx = s->eye_x + look_x * CLAWT_FACE_LOOK_REACH;
    cy = s->eye_y + look_y * CLAWT_FACE_LOOK_REACH * FACE_LOOK_REACH_Y;

    /* A blink squashes the eye toward its own middle, down to a line. */
    height = FACE_EYE_HEIGHT - (FACE_EYE_HEIGHT - FACE_EYE_MIN) * blink;

    out->left.x = cx - s->eye_gap;
    out->right.x = cx + s->eye_gap;
    out->left.y = out->right.y = cy;
    out->left.width = out->right.width = FACE_EYE_WIDTH;
    out->left.height = out->right.height = height;
    out->left.radius = out->right.radius = MIN(FACE_EYE_WIDTH, height) / 2.0;
}

void
clawt_face_look_at(gdouble center_x, gdouble center_y, gdouble radius,
                   gdouble pointer_x, gdouble pointer_y,
                   gdouble *look_x, gdouble *look_y)
{
    gdouble dx = pointer_x - center_x;
    gdouble dy = pointer_y - center_y;
    gdouble distance = sqrt(dx * dx + dy * dy);
    gdouble reach;

    g_return_if_fail(look_x != NULL && look_y != NULL);

    *look_x = 0.0;
    *look_y = 0.0;

    if (!(radius > 0.0) || !(distance > 1e-9) || !isfinite(distance))
        return;

    /* Grows with distance, full at FACE_LOOK_SPAN radii: gentle up close. */
    reach = MIN(1.0, distance / (radius * FACE_LOOK_SPAN));
    *look_x = dx / distance * reach;
    *look_y = dy / distance * reach;
}

gdouble
clawt_face_ease(gdouble current, gdouble target, gdouble dt_ms)
{
    gdouble next;

    if (!(dt_ms > 0.0))
        return current;

    next = current + (target - current) * (1.0 - exp(-dt_ms / FACE_EASE_MS));

    if (fabs(target - next) < 0.002)
        return target;

    return next;
}

/* ── Blinking ─────────────────────────────────────────────────────── */

guint
clawt_face_blink_period(guint32 seed)
{
    return FACE_BLINK_BASE_MS + seed % FACE_BLINK_SPREAD;
}

/* Where in its own cycle an agent's blinks fall, so agents are not in step. */
static gint64
blink_phase(guint32 seed)
{
    return (gint64)(mix32(seed ^ 0xa5a5a5a5U) % clawt_face_blink_period(seed));
}

/* The per-blink random number: jitter, and whether it doubles. */
static guint32
blink_roll(guint32 seed, gint64 cycle)
{
    return mix32(seed ^ mix32((guint32)cycle * 0x9e3779b9U) ^
                 (guint32)(cycle >> 32));
}

/* Floor division that rounds toward minus infinity for negative times. */
static gint64
floor_div(gint64 a, gint64 b)
{
    gint64 q = a / b;

    if ((a % b != 0) && ((a < 0) != (b < 0)))
        q--;

    return q;
}

/* When the first blink of @cycle starts. */
static gint64
blink_start(guint32 seed, gint64 cycle)
{
    gint64 period = clawt_face_blink_period(seed);

    return cycle * period + blink_phase(seed) +
           (gint64)(blink_roll(seed, cycle) % FACE_BLINK_JITTER);
}

static gboolean
blink_doubles(guint32 seed, gint64 cycle)
{
    return (blink_roll(seed, cycle) >> 12) % 5 == 0;
}

/* A triangle: shut at the middle of a CLAWT_FACE_BLINK_MS window. */
static gdouble
blink_curve(gint64 since)
{
    gdouble half = CLAWT_FACE_BLINK_MS / 2.0;

    if (since < 0 || since >= CLAWT_FACE_BLINK_MS)
        return 0.0;

    if (since < half)
        return since / half;

    return (CLAWT_FACE_BLINK_MS - since) / half;
}

gdouble
clawt_face_blink_at(guint32 seed, gint64 t_ms)
{
    gint64 period = clawt_face_blink_period(seed);
    gint64 cycle = floor_div(t_ms - blink_phase(seed), period);
    gdouble amount = 0.0;
    gint64 c;

    /*
     * A blink starts in its own cycle but can run past the cycle's end
     * (jitter plus a double blink), so the previous cycle is checked too.
     */
    for (c = cycle - 1; c <= cycle; c++) {
        gint64 start = blink_start(seed, c);

        amount = MAX(amount, blink_curve(t_ms - start));
        if (blink_doubles(seed, c))
            amount = MAX(amount,
                         blink_curve(t_ms - start - FACE_DOUBLE_GAP_MS));
    }

    return amount;
}

gint64
clawt_face_next_blink(guint32 seed, gint64 t_ms)
{
    gint64 period = clawt_face_blink_period(seed);
    gint64 cycle = floor_div(t_ms - blink_phase(seed), period);
    gint64 best = G_MAXINT64;
    gint64 c;

    for (c = cycle - 1; c <= cycle + 2; c++) {
        gint64 start = blink_start(seed, c);

        if (start >= t_ms && start < best)
            best = start;
        if (blink_doubles(seed, c) && start + FACE_DOUBLE_GAP_MS >= t_ms &&
            start + FACE_DOUBLE_GAP_MS < best)
            best = start + FACE_DOUBLE_GAP_MS;
    }

    return best;
}

/* ── Colour ───────────────────────────────────────────────────────── */

/* #rgb or #rrggbb into 0..1 channels; FALSE for anything else. */
static gboolean
parse_hex(const gchar *hex, gdouble rgb[3])
{
    gsize digits;
    guint i;

    if (clawt_color_ink(hex) == NULL)
        return FALSE;

    digits = strlen(hex + 1);

    for (i = 0; i < 3; i++) {
        gint v;

        if (digits == 3)
            v = g_ascii_xdigit_value(hex[1 + i]) * 17;
        else
            v = g_ascii_xdigit_value(hex[1 + i * 2]) * 16 +
                g_ascii_xdigit_value(hex[2 + i * 2]);
        rgb[i] = v / 255.0;
    }

    return TRUE;
}

/* sRGB to linear light, as WCAG's relative luminance needs. */
static gdouble
linear(gdouble c)
{
    return (c <= 0.04045) ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

const gchar *
clawt_face_eye_color(const gchar *fill)
{
    gdouble rgb[3];
    gdouble luminance;

    if (!parse_hex(fill, rgb))
        return "#1b1b22";

    luminance = 0.2126 * linear(rgb[0]) + 0.7152 * linear(rgb[1]) +
                0.0722 * linear(rgb[2]);

    /*
     * The fill where both eye colours have the same WCAG contrast:
     * (L + .05)² = (.011 + .05)(.91 + .05) for the two inks below,
     * so L ≈ .19.  Below it white eyes stand out more, above it dark.
     */
    return (luminance < 0.19) ? "#f6f5f4" : "#1b1b22";
}

const gchar *
clawt_face_palette_color(guint index)
{
    if (index >= CLAWT_FACE_PALETTE_SIZE)
        return NULL;

    return face_palette[index];
}

void
clawt_face_pick_random(gint *shape, const gchar **color)
{
    /*
     * g_random: a process-wide generator seeded from the system, which
     * is all this needs -- nothing about a face is secret, it only has
     * to differ from the last one.
     */
    if (shape != NULL)
        *shape = g_random_int_range(0, (gint32)FACE_SHAPE_COUNT);
    if (color != NULL)
        *color = face_palette[g_random_int_range(0, CLAWT_FACE_PALETTE_SIZE)];
}

/* ── SVG ──────────────────────────────────────────────────────────── */

/* A number in the C locale: a decimal comma would end the path there. */
static void
append_number(GString *out, gdouble value)
{
    gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];

    if (fabs(value) < 0.0005)
        value = 0.0;
    g_string_append(out, g_ascii_formatd(buffer, sizeof(buffer), "%.3f",
                                         value));
}

static void
append_eye(GString *out, const ClawtFaceEye *eye)
{
    g_string_append(out, "<rect class=\"face-eye\" x=\"");
    append_number(out, eye->x - eye->width / 2.0);
    g_string_append(out, "\" y=\"");
    append_number(out, eye->y - eye->height / 2.0);
    g_string_append(out, "\" width=\"");
    append_number(out, eye->width);
    g_string_append(out, "\" height=\"");
    append_number(out, eye->height);
    g_string_append(out, "\" rx=\"");
    append_number(out, eye->radius);
    g_string_append(out, "\"/>");
}

gchar *
clawt_face_svg(gint shape, const gchar *fill, guint size,
               const ClawtFacePose *pose, guint32 seed, gboolean animated)
{
    enum { POINTS = 120 };
    gdouble xy[POINTS * 2];
    ClawtFaceEyes eyes;
    GString *out;
    guint i;

    /* The colour goes into markup: only a real colour is let through. */
    if (clawt_color_ink(fill) == NULL)
        return NULL;

    clawt_face_outline(shape, POINTS, xy);

    out = g_string_new(NULL);
    g_string_append_printf(
        out,
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"-1 -1 2 2\" "
        "width=\"%u\" height=\"%u\" class=\"face\" aria-hidden=\"true\"",
        size, size);

    /*
     * The web stylesheet runs the blink as a CSS animation; these give
     * each face its own period and a start offset, so a sidebar of
     * agents never blinks in step.  The delay is negative so the face is
     * already partway through its cycle when the page loads.
     */
    if (animated) {
        guint period = clawt_face_blink_period(seed);

        g_string_append_printf(out,
                               " style=\"--blink-gap:%ums;--blink-delay:-%ums\"",
                               period, (guint)blink_phase(seed));
    }
    g_string_append(out, ">");

    g_string_append_printf(out, "<path class=\"face-body\" fill=\"%s\" d=\"M",
                           fill);
    for (i = 0; i < POINTS; i++) {
        if (i > 0)
            g_string_append(out, " L");
        g_string_append_c(out, ' ');
        append_number(out, xy[i * 2]);
        g_string_append_c(out, ' ');
        append_number(out, xy[i * 2 + 1]);
    }
    g_string_append(out, " Z\"/>");

    clawt_face_eyes(shape, pose, &eyes);
    g_string_append_printf(out, "<g class=\"face-eyes\" fill=\"%s\">",
                           clawt_face_eye_color(fill));
    append_eye(out, &eyes.left);
    append_eye(out, &eyes.right);
    g_string_append(out, "</g></svg>");

    return g_string_free(out, FALSE);
}
