/*
 * gtk-face.c - An agent's generated face, and the picker for one
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Cost is the design constraint here: a sidebar can hold dozens of
 * faces, and a face that ran its frame clock all the time would keep
 * the whole window redrawing at 60 fps to show nothing happening.  So a
 * face is idle almost always.  Between blinks it holds one GLib timeout,
 * armed for the moment clawt_face_next_blink() names; the frame clock
 * runs only for the 160 ms of a blink and while the eyes are easing
 * toward the pointer, and stops the moment both are done.  Nothing at
 * all runs while a face is unmapped.
 */

#include "clawt-gtk.h"
#include "clawt-gtk-face.h"

#include <adwaita.h>
#include <clawtilla.h>
#include <math.h>

/* defines */
#define FACE_OUTLINE_POINTS (96)
#define FACE_FALLBACK_COLOR "#7d7f86"
#define FACE_DEFAULT_DT_MS  (16.0)
#define PICKER_PREVIEW_SIZE (96)
#define PICKER_TILE_SIZE    (36)
#define PICKER_SWATCH_SIZE  (22)

struct _ClawtGtkFace {
    GtkWidget  parent_instance;

    gchar     *agent_id;
    guint32    seed;
    gint       shape;
    GdkRGBA    fill;
    GdkRGBA    eye;
    gint       size;

    gdouble    look_x;
    gdouble    look_y;
    gdouble    target_x;
    gdouble    target_y;
    gdouble    blink;

    gint64     blink_until;   /* ms: keep the clock running until then */
    guint      blink_source;  /* the timeout waiting for the next blink */
    guint      tick_id;       /* the frame clock, while anything moves */
    gint64     last_us;       /* the previous frame, for easing */
};

G_DEFINE_FINAL_TYPE(ClawtGtkFace, clawt_gtk_face, GTK_TYPE_WIDGET)

/*
 * Every live face, so the client-wide animation switch reaches faces
 * already on screen.  Borrowed pointers: a face removes itself in
 * dispose.
 */
static GHashTable *live_faces = NULL;
static gboolean faces_animate = TRUE;

/* agent id -> resolved face name, from agent.list */
static GHashTable *noted_faces = NULL;

static gint64
now_ms(void)
{
    return g_get_monotonic_time() / 1000;
}

/* ── Colour ───────────────────────────────────────────────────────── */

/*
 * The body and eye colours.  Only a colour clawt_color_ink() accepts is
 * parsed -- the same check every other face path makes -- and anything
 * else is the neutral grey, never black or a failed parse's zeroes.
 */
static void
apply_color(ClawtGtkFace *self, const gchar *color)
{
    const gchar *use = (clawt_color_ink(color) != NULL) ? color
                                                        : FACE_FALLBACK_COLOR;

    gdk_rgba_parse(&self->fill, use);
    gdk_rgba_parse(&self->eye, clawt_face_eye_color(use));
}

/* ── Motion ───────────────────────────────────────────────────────── */

static void arm_blink(ClawtGtkFace *self);

gboolean
clawt_gtk_face_step(ClawtGtkFace *self, gdouble dt_ms)
{
    g_return_val_if_fail(CLAWT_GTK_IS_FACE(self), FALSE);

    self->look_x = clawt_face_ease(self->look_x, self->target_x, dt_ms);
    self->look_y = clawt_face_ease(self->look_y, self->target_y, dt_ms);

    return self->look_x != self->target_x || self->look_y != self->target_y;
}

/*
 * One frame: ease the eyes, take the blink from the shared schedule,
 * draw.  Stops itself once the eyes have arrived and no blink is in its
 * window -- and re-arms the blink timeout on the way out, which is the
 * only thing left running.
 */
static gboolean
on_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer user_data)
{
    ClawtGtkFace *self = CLAWT_GTK_FACE(widget);
    gint64 now_us = gdk_frame_clock_get_frame_time(clock);
    gdouble dt = (self->last_us > 0) ? (now_us - self->last_us) / 1000.0
                                     : FACE_DEFAULT_DT_MS;
    gint64 now = now_us / 1000;
    gboolean moving;
    gboolean blinking;

    (void)user_data;

    self->last_us = now_us;
    moving = clawt_gtk_face_step(self, dt);
    self->blink = faces_animate ? clawt_face_blink_at(self->seed, now) : 0.0;
    gtk_widget_queue_draw(widget);

    blinking = self->blink > 0.0 || now < self->blink_until;

    if (moving || blinking)
        return G_SOURCE_CONTINUE;

    self->tick_id = 0;
    self->last_us = 0;
    arm_blink(self);

    return G_SOURCE_REMOVE;
}

static void
ensure_ticking(ClawtGtkFace *self)
{
    if (self->tick_id != 0 || !gtk_widget_get_mapped(GTK_WIDGET(self)))
        return;

    self->last_us = 0;
    self->tick_id = gtk_widget_add_tick_callback(GTK_WIDGET(self), on_tick,
                                                 NULL, NULL);
}

static gboolean
on_blink_due(gpointer user_data)
{
    ClawtGtkFace *self = user_data;

    self->blink_source = 0;

    /*
     * Long enough for a double blink's second half too; the schedule
     * itself says when the eyes are open, this only keeps the clock
     * running to see it.
     */
    self->blink_until = now_ms() + CLAWT_FACE_BLINK_MS + 300;
    ensure_ticking(self);

    return G_SOURCE_REMOVE;
}

/* One timeout, at the next blink -- the whole cost of an idle face. */
static void
arm_blink(ClawtGtkFace *self)
{
    gint64 now;
    gint64 next;

    g_clear_handle_id(&self->blink_source, g_source_remove);

    if (!faces_animate || !gtk_widget_get_mapped(GTK_WIDGET(self)))
        return;

    now = now_ms();
    next = clawt_face_next_blink(self->seed, now);
    self->blink_source = g_timeout_add((guint)CLAMP(next - now, 0, G_MAXINT),
                                       on_blink_due, self);
}

/* Everything back to rest, nothing running. */
static void
settle(ClawtGtkFace *self)
{
    g_clear_handle_id(&self->blink_source, g_source_remove);

    if (self->tick_id != 0) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->tick_id);
        self->tick_id = 0;
    }

    self->look_x = self->look_y = 0.0;
    self->target_x = self->target_y = 0.0;
    self->blink = 0.0;
    self->blink_until = 0;
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void
clawt_gtk_face_point_at(ClawtGtkFace *self, gdouble x, gdouble y)
{
    gdouble width;
    gdouble height;

    g_return_if_fail(CLAWT_GTK_IS_FACE(self));

    if (!faces_animate)
        return;

    /* Before the first allocation, the size asked for stands in. */
    width = gtk_widget_get_width(GTK_WIDGET(self));
    height = gtk_widget_get_height(GTK_WIDGET(self));
    if (width <= 0 || height <= 0)
        width = height = self->size;

    clawt_face_look_at(width / 2.0, height / 2.0, MIN(width, height) / 2.0,
                       x, y, &self->target_x, &self->target_y);
    ensure_ticking(self);
}

void
clawt_gtk_face_look_away(ClawtGtkFace *self)
{
    g_return_if_fail(CLAWT_GTK_IS_FACE(self));

    self->target_x = 0.0;
    self->target_y = 0.0;
    ensure_ticking(self);
}

/*
 * The pointer over the tracked area, translated into the face's own
 * coordinates -- the area may be a whole row, and the eyes should look
 * at where the pointer really is, not where it is in the row.
 */
static void
on_motion(GtkEventControllerMotion *motion, gdouble x, gdouble y,
          gpointer user_data)
{
    ClawtGtkFace *self = user_data;
    GtkWidget *area =
        gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(motion));
    graphene_point_t from = GRAPHENE_POINT_INIT((gfloat)x, (gfloat)y);
    graphene_point_t to;

    if (area == GTK_WIDGET(self)) {
        clawt_gtk_face_point_at(self, x, y);
        return;
    }

    if (gtk_widget_compute_point(area, GTK_WIDGET(self), &from, &to))
        clawt_gtk_face_point_at(self, to.x, to.y);
}

static void
on_leave(GtkEventControllerMotion *motion, gpointer user_data)
{
    (void)motion;
    clawt_gtk_face_look_away(CLAWT_GTK_FACE(user_data));
}

void
clawt_gtk_face_track(ClawtGtkFace *self, GtkWidget *area)
{
    GtkEventController *motion;

    g_return_if_fail(CLAWT_GTK_IS_FACE(self));
    g_return_if_fail(GTK_IS_WIDGET(area));

    motion = gtk_event_controller_motion_new();

    /*
     * connect_object: the controller lives as long as @area, which may
     * outlive the face (a row rebuilt with a new face in it); the
     * handlers must go with the face.
     */
    g_signal_connect_object(motion, "enter", G_CALLBACK(on_motion), self, 0);
    g_signal_connect_object(motion, "motion", G_CALLBACK(on_motion), self, 0);
    g_signal_connect_object(motion, "leave", G_CALLBACK(on_leave), self, 0);
    gtk_widget_add_controller(area, motion);
}

void
clawt_gtk_face_get_pose(ClawtGtkFace *self, gdouble *look_x, gdouble *look_y,
                        gdouble *target_x, gdouble *target_y)
{
    g_return_if_fail(CLAWT_GTK_IS_FACE(self));

    if (look_x != NULL)
        *look_x = self->look_x;
    if (look_y != NULL)
        *look_y = self->look_y;
    if (target_x != NULL)
        *target_x = self->target_x;
    if (target_y != NULL)
        *target_y = self->target_y;
}

gboolean
clawt_gtk_face_is_blink_armed(ClawtGtkFace *self)
{
    g_return_val_if_fail(CLAWT_GTK_IS_FACE(self), FALSE);

    return self->blink_source != 0;
}

void
clawt_gtk_faces_set_animate(gboolean animate)
{
    GHashTableIter iter;
    gpointer face;

    animate = animate ? TRUE : FALSE;
    if (faces_animate == animate)
        return;

    faces_animate = animate;

    if (live_faces == NULL)
        return;

    g_hash_table_iter_init(&iter, live_faces);
    while (g_hash_table_iter_next(&iter, &face, NULL)) {
        if (animate)
            arm_blink(CLAWT_GTK_FACE(face));
        else
            settle(CLAWT_GTK_FACE(face));
    }
}

gboolean
clawt_gtk_faces_get_animate(void)
{
    return faces_animate;
}

/* ── Which face ───────────────────────────────────────────────────── */

void
clawt_gtk_face_note(const gchar *agent_id, const gchar *face)
{
    g_return_if_fail(agent_id != NULL);

    if (noted_faces == NULL)
        noted_faces = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                            g_free);

    if (face == NULL)
        g_hash_table_remove(noted_faces, agent_id);
    else
        g_hash_table_replace(noted_faces, g_strdup(agent_id), g_strdup(face));
}

gint
clawt_gtk_face_for(const gchar *agent_id)
{
    const gchar *noted = NULL;
    gint shape;

    if (noted_faces != NULL && agent_id != NULL)
        noted = g_hash_table_lookup(noted_faces, agent_id);

    if (noted == NULL)
        return clawt_face_resolve(agent_id, NULL);

    /*
     * A name this build's table does not have -- a newer daemon with a
     * shape added -- is the letter disc, which is always drawable,
     * rather than a different shape that would disagree with the
     * clients that do know it.
     */
    shape = clawt_face_shape_lookup(noted);

    return (shape == -2) ? CLAWT_FACE_INITIALS : shape;
}

/* ── The widget ───────────────────────────────────────────────────── */

static void
clawt_gtk_face_measure(GtkWidget *widget, GtkOrientation orientation,
                       gint for_size, gint *minimum, gint *natural,
                       gint *minimum_baseline, gint *natural_baseline)
{
    ClawtGtkFace *self = CLAWT_GTK_FACE(widget);

    (void)orientation;
    (void)for_size;

    *minimum = *natural = self->size;
    *minimum_baseline = *natural_baseline = -1;
}

/* One eye as a rounded rect in widget coordinates, added to @builder. */
static void
add_eye(GskPathBuilder *builder, const ClawtFaceEye *eye, gdouble cx,
        gdouble cy, gdouble scale)
{
    GskRoundedRect rounded;
    graphene_rect_t rect = GRAPHENE_RECT_INIT(
        (gfloat)(cx + (eye->x - eye->width / 2.0) * scale),
        (gfloat)(cy + (eye->y - eye->height / 2.0) * scale),
        (gfloat)(eye->width * scale),
        (gfloat)(eye->height * scale));

    gsk_rounded_rect_init_from_rect(&rounded, &rect,
                                    (gfloat)(eye->radius * scale));
    gsk_path_builder_add_rounded_rect(builder, &rounded);
}

static void
clawt_gtk_face_snapshot(GtkWidget *widget, GtkSnapshot *snapshot)
{
    ClawtGtkFace *self = CLAWT_GTK_FACE(widget);
    gdouble width = gtk_widget_get_width(widget);
    gdouble height = gtk_widget_get_height(widget);
    gdouble scale = MIN(width, height) / 2.0;
    gdouble cx = width / 2.0;
    gdouble cy = height / 2.0;
    gdouble xy[FACE_OUTLINE_POINTS * 2];
    ClawtFacePose pose;
    ClawtFaceEyes eyes;
    GskPathBuilder *builder;
    GskPath *path;
    guint i;

    if (scale <= 0.0)
        return;

    /* ── The body: one closed path round the star-shaped outline ── */
    clawt_face_outline(self->shape, FACE_OUTLINE_POINTS, xy);
    builder = gsk_path_builder_new();
    for (i = 0; i < FACE_OUTLINE_POINTS; i++) {
        gfloat x = (gfloat)(cx + xy[i * 2] * scale);
        gfloat y = (gfloat)(cy + xy[i * 2 + 1] * scale);

        if (i == 0)
            gsk_path_builder_move_to(builder, x, y);
        else
            gsk_path_builder_line_to(builder, x, y);
    }
    gsk_path_builder_close(builder);
    path = gsk_path_builder_free_to_path(builder);
    gtk_snapshot_append_fill(snapshot, path, GSK_FILL_RULE_WINDING,
                             &self->fill);
    gsk_path_unref(path);

    /* ── The eyes, in the current pose ── */
    pose.look_x = self->look_x;
    pose.look_y = self->look_y;
    pose.blink = self->blink;
    clawt_face_eyes(self->shape, &pose, &eyes);

    builder = gsk_path_builder_new();
    add_eye(builder, &eyes.left, cx, cy, scale);
    add_eye(builder, &eyes.right, cx, cy, scale);
    path = gsk_path_builder_free_to_path(builder);
    gtk_snapshot_append_fill(snapshot, path, GSK_FILL_RULE_WINDING,
                             &self->eye);
    gsk_path_unref(path);
}

static void
clawt_gtk_face_map(GtkWidget *widget)
{
    GTK_WIDGET_CLASS(clawt_gtk_face_parent_class)->map(widget);
    arm_blink(CLAWT_GTK_FACE(widget));
}

static void
clawt_gtk_face_unmap(GtkWidget *widget)
{
    settle(CLAWT_GTK_FACE(widget));
    GTK_WIDGET_CLASS(clawt_gtk_face_parent_class)->unmap(widget);
}

static void
clawt_gtk_face_dispose(GObject *object)
{
    ClawtGtkFace *self = CLAWT_GTK_FACE(object);

    g_clear_handle_id(&self->blink_source, g_source_remove);
    if (live_faces != NULL)
        g_hash_table_remove(live_faces, self);

    G_OBJECT_CLASS(clawt_gtk_face_parent_class)->dispose(object);
}

static void
clawt_gtk_face_finalize(GObject *object)
{
    ClawtGtkFace *self = CLAWT_GTK_FACE(object);

    g_free(self->agent_id);

    G_OBJECT_CLASS(clawt_gtk_face_parent_class)->finalize(object);
}

static void
clawt_gtk_face_class_init(ClawtGtkFaceClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = clawt_gtk_face_dispose;
    object_class->finalize = clawt_gtk_face_finalize;
    widget_class->measure = clawt_gtk_face_measure;
    widget_class->snapshot = clawt_gtk_face_snapshot;
    widget_class->map = clawt_gtk_face_map;
    widget_class->unmap = clawt_gtk_face_unmap;

    gtk_widget_class_set_css_name(widget_class, "clawt-face");
    gtk_widget_class_set_accessible_role(widget_class, GTK_ACCESSIBLE_ROLE_IMG);
}

static void
clawt_gtk_face_init(ClawtGtkFace *self)
{
    self->size = 32;
    apply_color(self, NULL);

    if (live_faces == NULL)
        live_faces = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_hash_table_add(live_faces, self);
}

GtkWidget *
clawt_gtk_face_new(const gchar *agent_id, gint shape, const gchar *color,
                   gint size)
{
    ClawtGtkFace *self = g_object_new(CLAWT_GTK_TYPE_FACE, NULL);

    self->agent_id = g_strdup(agent_id);
    self->seed = clawt_face_seed(agent_id);
    self->shape = shape;
    self->size = MAX(size, 1);
    apply_color(self, color);

    /* over the face itself always counts; a row can be added */
    clawt_gtk_face_track(self, GTK_WIDGET(self));

    return GTK_WIDGET(self);
}

void
clawt_gtk_face_set_shape(ClawtGtkFace *self, gint shape)
{
    g_return_if_fail(CLAWT_GTK_IS_FACE(self));

    self->shape = shape;
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

gint
clawt_gtk_face_get_shape(ClawtGtkFace *self)
{
    g_return_val_if_fail(CLAWT_GTK_IS_FACE(self), 0);

    return self->shape;
}

void
clawt_gtk_face_set_color(ClawtGtkFace *self, const gchar *color)
{
    g_return_if_fail(CLAWT_GTK_IS_FACE(self));

    apply_color(self, color);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void
clawt_gtk_face_set_agent_id(ClawtGtkFace *self, const gchar *agent_id)
{
    g_return_if_fail(CLAWT_GTK_IS_FACE(self));

    g_free(self->agent_id);
    self->agent_id = g_strdup(agent_id);
    self->seed = clawt_face_seed(agent_id);
    arm_blink(self);
}

/* ── The picker ───────────────────────────────────────────────────── */

struct _ClawtGtkFacePicker {
    GtkBox        parent_instance;

    gchar        *agent_id;
    gchar        *face;      /* "" automatic, a shape name, or initials */
    gchar        *color;     /* "" automatic, or #rrggbb */

    GtkWidget    *stack;     /* the preview: a face, or the letter disc */
    ClawtGtkFace *preview;
    GtkWidget    *initials;
    GtkWidget    *caption;
    GtkWidget    *shapes;    /* GtkFlowBox */
    GtkWidget    *swatches;  /* GtkFlowBox */
    GtkWidget    *custom;    /* "+": any other colour */
    GPtrArray    *tiles;     /* ClawtGtkFace *, borrowed, to recolour */
    ClawtGtkFace *auto_tile;

    gboolean      updating;  /* selecting in code: not a click */
};

G_DEFINE_FINAL_TYPE(ClawtGtkFacePicker, clawt_gtk_face_picker, GTK_TYPE_BOX)

enum { PICKER_CHANGED, PICKER_N_SIGNALS };
static guint picker_signals[PICKER_N_SIGNALS];

/* The colour a face is painted in: chosen, or the agent's derived tone. */
static const gchar *
picker_paint(ClawtGtkFacePicker *self)
{
    if (self->color != NULL && *self->color != '\0')
        return self->color;

    return clawt_avatar_color(self->agent_id, self->agent_id, NULL);
}

/* The shape the current choice draws: chosen, or the automatic one. */
static gint
picker_shape(ClawtGtkFacePicker *self)
{
    if (self->face == NULL || *self->face == '\0')
        return clawt_face_resolve(self->agent_id, NULL);

    return clawt_face_resolve(self->agent_id, self->face);
}

/* Redraws the preview, every tile and the caption from the choice. */
static void
picker_refresh(ClawtGtkFacePicker *self)
{
    const gchar *paint = picker_paint(self);
    gint shape = picker_shape(self);
    gboolean automatic = (self->face == NULL || *self->face == '\0');
    g_autofree gchar *caption = NULL;
    guint i;

    for (i = 0; i < self->tiles->len; i++)
        clawt_gtk_face_set_color(g_ptr_array_index(self->tiles, i), paint);

    clawt_gtk_face_set_shape(self->auto_tile,
                             clawt_face_resolve(self->agent_id, NULL));

    if (shape == CLAWT_FACE_INITIALS) {
        gtk_stack_set_visible_child(GTK_STACK(self->stack), self->initials);
        adw_avatar_set_text(ADW_AVATAR(self->initials),
                            (self->agent_id != NULL && *self->agent_id)
                                ? self->agent_id : "Agent");
        caption = g_strdup("Initials");
    } else {
        gtk_stack_set_visible_child(GTK_STACK(self->stack),
                                    GTK_WIDGET(self->preview));
        clawt_gtk_face_set_shape(self->preview, shape);
        clawt_gtk_face_set_color(self->preview, paint);
        caption = automatic
            ? g_strdup_printf("%s (automatic)",
                              clawt_face_shape_label(shape))
            : g_strdup(clawt_face_shape_label(shape));
    }

    gtk_label_set_text(GTK_LABEL(self->caption), caption);
}

static void
picker_changed(ClawtGtkFacePicker *self)
{
    picker_refresh(self);
    if (!self->updating)
        g_signal_emit(self, picker_signals[PICKER_CHANGED], 0);
}

static void
on_shape_selected(GtkFlowBox *box, GtkFlowBoxChild *child, gpointer user_data)
{
    ClawtGtkFacePicker *self = user_data;
    const gchar *value = g_object_get_data(G_OBJECT(child), "face-value");

    (void)box;

    g_free(self->face);
    self->face = g_strdup(value != NULL ? value : "");
    picker_changed(self);
}

static void
on_swatch_selected(GtkFlowBox *box, GtkFlowBoxChild *child, gpointer user_data)
{
    ClawtGtkFacePicker *self = user_data;
    const gchar *value = g_object_get_data(G_OBJECT(child), "color-value");

    (void)box;

    g_free(self->color);
    self->color = g_strdup(value != NULL ? value : "");
    picker_changed(self);
}

/* A custom colour: as #rrggbb, the one spelling every client takes. */
static void
on_custom_chosen(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ClawtGtkFacePicker *self = user_data;
    g_autoptr(GdkRGBA) rgba = NULL;

    rgba = gtk_color_dialog_choose_rgba_finish(GTK_COLOR_DIALOG(source),
                                               result, NULL);
    if (rgba != NULL) {
        g_free(self->color);
        self->color = g_strdup_printf("#%02x%02x%02x",
                                      (guint)lround(rgba->red * 255.0),
                                      (guint)lround(rgba->green * 255.0),
                                      (guint)lround(rgba->blue * 255.0));
        gtk_flow_box_unselect_all(GTK_FLOW_BOX(self->swatches));
        picker_changed(self);
    }

    g_object_unref(self);
}

/*
 * A plain "+" rather than a GtkColorDialogButton: that one always shows
 * a colour, and before anything is chosen the colour it shows is GTK's
 * default -- which reads as a choice somebody already made.
 */
static void
on_custom_clicked(GtkButton *button, gpointer user_data)
{
    ClawtGtkFacePicker *self = user_data;
    g_autoptr(GtkColorDialog) dialog = gtk_color_dialog_new();
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(button));
    GdkRGBA start;

    gtk_color_dialog_set_title(dialog, "Face colour");
    gtk_color_dialog_set_with_alpha(dialog, FALSE);
    gdk_rgba_parse(&start, picker_paint(self));
    gtk_color_dialog_choose_rgba(dialog,
                                 GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL,
                                 &start, NULL, on_custom_chosen,
                                 g_object_ref(self));
}

/* A swatch: a filled circle; the automatic one with a dashed ring. */
static void
draw_swatch(GtkDrawingArea *area, cairo_t *cr, gint width, gint height,
            gpointer user_data)
{
    ClawtGtkFacePicker *self = user_data;
    const gchar *value = g_object_get_data(G_OBJECT(area), "color-value");
    gboolean automatic = (value == NULL || *value == '\0');
    GdkRGBA rgba;
    gdouble radius = MIN(width, height) / 2.0 - 1.5;

    gdk_rgba_parse(&rgba, automatic
                              ? clawt_avatar_color(self->agent_id,
                                                   self->agent_id, NULL)
                              : value);
    gdk_cairo_set_source_rgba(cr, &rgba);
    cairo_arc(cr, width / 2.0, height / 2.0, radius, 0, 2 * G_PI);
    cairo_fill(cr);

    if (automatic) {
        static const gdouble dash[] = { 2.5, 2.5 };

        cairo_set_source_rgba(cr, 1, 1, 1, 0.85);
        cairo_set_line_width(cr, 1.5);
        cairo_set_dash(cr, dash, 2, 0);
        cairo_arc(cr, width / 2.0, height / 2.0, radius - 3.0, 0, 2 * G_PI);
        cairo_stroke(cr);
    }
}

static GtkWidget *
swatch_new(ClawtGtkFacePicker *self, const gchar *value, const gchar *tooltip)
{
    GtkWidget *area = gtk_drawing_area_new();
    GtkWidget *child = gtk_flow_box_child_new();

    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(area),
                                       PICKER_SWATCH_SIZE);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(area),
                                        PICKER_SWATCH_SIZE);
    g_object_set_data_full(G_OBJECT(area), "color-value", g_strdup(value),
                           g_free);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), draw_swatch, self,
                                   NULL);

    gtk_flow_box_child_set_child(GTK_FLOW_BOX_CHILD(child), area);
    g_object_set_data_full(G_OBJECT(child), "color-value", g_strdup(value),
                           g_free);
    gtk_widget_set_tooltip_text(child, tooltip);
    gtk_widget_add_css_class(child, "clawt-face-swatch");

    return child;
}

static GtkWidget *
tile_new(const gchar *value, GtkWidget *content, const gchar *tooltip)
{
    GtkWidget *child = gtk_flow_box_child_new();

    gtk_widget_set_halign(content, GTK_ALIGN_CENTER);
    gtk_flow_box_child_set_child(GTK_FLOW_BOX_CHILD(child), content);
    g_object_set_data_full(G_OBJECT(child), "face-value", g_strdup(value),
                           g_free);
    gtk_widget_set_tooltip_text(child, tooltip);
    gtk_widget_add_css_class(child, "clawt-face-tile");

    return child;
}

/* Selects the child of @box whose @key data equals @value, if any. */
static gboolean
select_by(GtkWidget *box, const gchar *key, const gchar *value)
{
    GtkWidget *child;

    for (child = gtk_widget_get_first_child(box); child != NULL;
         child = gtk_widget_get_next_sibling(child)) {
        const gchar *have = g_object_get_data(G_OBJECT(child), key);

        if (GTK_IS_FLOW_BOX_CHILD(child) &&
            g_strcmp0(have != NULL ? have : "", value) == 0) {
            gtk_flow_box_select_child(GTK_FLOW_BOX(box),
                                      GTK_FLOW_BOX_CHILD(child));
            return TRUE;
        }
    }

    gtk_flow_box_unselect_all(GTK_FLOW_BOX(box));
    return FALSE;
}

/* The stored spelling of a face value: lowercase, trimmed, "" if unknown. */
static gchar *
normalise_face(const gchar *face)
{
    gint shape = clawt_face_shape_lookup(face);

    if (shape == CLAWT_FACE_INITIALS)
        return g_strdup(CLAWT_FACE_INITIALS_NAME);
    if (shape >= 0)
        return g_strdup(clawt_face_shape_name(shape));

    return g_strdup("");
}

void
clawt_gtk_face_picker_select(ClawtGtkFacePicker *self, const gchar *face,
                             const gchar *color)
{
    g_return_if_fail(CLAWT_GTK_IS_FACE_PICKER(self));

    self->updating = TRUE;

    g_free(self->face);
    self->face = normalise_face(face);
    select_by(self->shapes, "face-value", self->face);

    g_free(self->color);
    self->color = g_strdup(clawt_color_ink(color) != NULL ? color : "");

    /* a colour that is not a swatch shows in the preview alone */
    select_by(self->swatches, "color-value", self->color);

    self->updating = FALSE;
    picker_refresh(self);
    g_signal_emit(self, picker_signals[PICKER_CHANGED], 0);
}

void
clawt_gtk_face_picker_set_agent_id(ClawtGtkFacePicker *self,
                                   const gchar        *agent_id)
{
    g_return_if_fail(CLAWT_GTK_IS_FACE_PICKER(self));

    g_free(self->agent_id);
    self->agent_id = g_strdup(agent_id);
    clawt_gtk_face_set_agent_id(self->preview, agent_id);
    picker_refresh(self);
    gtk_widget_queue_draw(self->swatches);
}

const gchar *
clawt_gtk_face_picker_get_face(ClawtGtkFacePicker *self)
{
    g_return_val_if_fail(CLAWT_GTK_IS_FACE_PICKER(self), "");

    return self->face;
}

const gchar *
clawt_gtk_face_picker_get_color(ClawtGtkFacePicker *self)
{
    g_return_val_if_fail(CLAWT_GTK_IS_FACE_PICKER(self), "");

    return self->color;
}

ClawtGtkFace *
clawt_gtk_face_picker_get_preview(ClawtGtkFacePicker *self)
{
    g_return_val_if_fail(CLAWT_GTK_IS_FACE_PICKER(self), NULL);

    return self->preview;
}

static void
clawt_gtk_face_picker_finalize(GObject *object)
{
    ClawtGtkFacePicker *self = CLAWT_GTK_FACE_PICKER(object);

    g_free(self->agent_id);
    g_free(self->face);
    g_free(self->color);
    g_ptr_array_unref(self->tiles);

    G_OBJECT_CLASS(clawt_gtk_face_picker_parent_class)->finalize(object);
}

static void
clawt_gtk_face_picker_class_init(ClawtGtkFacePickerClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = clawt_gtk_face_picker_finalize;

    /**
     * ClawtGtkFacePicker::changed:
     * @picker: the picker
     *
     * The face or the colour changed, by a click or by
     * clawt_gtk_face_picker_select().
     */
    picker_signals[PICKER_CHANGED] = g_signal_new(
        "changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
        NULL, G_TYPE_NONE, 0);
}

static void
clawt_gtk_face_picker_init(ClawtGtkFacePicker *self)
{
    self->tiles = g_ptr_array_new();
    self->face = g_strdup("");
    self->color = g_strdup("");
}

GtkWidget *
clawt_gtk_face_picker_new(const gchar *agent_id, const gchar *face,
                          const gchar *color)
{
    ClawtGtkFacePicker *self = g_object_new(CLAWT_GTK_TYPE_FACE_PICKER,
                                            "orientation",
                                            GTK_ORIENTATION_VERTICAL,
                                            "spacing", 10, NULL);
    GtkWidget *row;
    guint i;

    self->agent_id = g_strdup(agent_id);
    gtk_widget_add_css_class(GTK_WIDGET(self), "clawt-face-picker");

    /* ── The preview, which watches the pointer anywhere over the picker ── */
    self->stack = gtk_stack_new();
    gtk_widget_set_halign(self->stack, GTK_ALIGN_CENTER);
    self->preview = CLAWT_GTK_FACE(clawt_gtk_face_new(
        agent_id, clawt_face_resolve(agent_id, NULL), NULL,
        PICKER_PREVIEW_SIZE));
    clawt_gtk_face_track(self->preview, GTK_WIDGET(self));
    self->initials = adw_avatar_new(PICKER_PREVIEW_SIZE, agent_id, TRUE);
    gtk_stack_add_child(GTK_STACK(self->stack), GTK_WIDGET(self->preview));
    gtk_stack_add_child(GTK_STACK(self->stack), self->initials);
    gtk_box_append(GTK_BOX(self), self->stack);

    self->caption = gtk_label_new(NULL);
    gtk_widget_add_css_class(self->caption, "dim-label");
    gtk_widget_add_css_class(self->caption, "caption");
    gtk_box_append(GTK_BOX(self), self->caption);

    /* ── Shapes: Automatic, each shape, Initials ── */
    self->shapes = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(self->shapes),
                                    GTK_SELECTION_SINGLE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(self->shapes), TRUE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(self->shapes), 4);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(self->shapes), 8);
    gtk_flow_box_set_activate_on_single_click(GTK_FLOW_BOX(self->shapes),
                                              TRUE);
    gtk_widget_set_halign(self->shapes, GTK_ALIGN_CENTER);

    self->auto_tile = CLAWT_GTK_FACE(clawt_gtk_face_new(
        agent_id, clawt_face_resolve(agent_id, NULL), NULL, PICKER_TILE_SIZE));
    g_ptr_array_add(self->tiles, self->auto_tile);
    {
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        GtkWidget *label = gtk_label_new("auto");

        gtk_widget_add_css_class(label, "caption");
        gtk_widget_add_css_class(label, "dim-label");
        gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->auto_tile));
        gtk_box_append(GTK_BOX(box), label);
        gtk_flow_box_append(GTK_FLOW_BOX(self->shapes),
                            tile_new("", box,
                                     "Automatic: picked from the agent's id"));
    }

    for (i = 0; i < clawt_face_shape_count(); i++) {
        GtkWidget *tile = clawt_gtk_face_new(agent_id, (gint)i, NULL,
                                             PICKER_TILE_SIZE);

        g_ptr_array_add(self->tiles, tile);
        gtk_flow_box_append(GTK_FLOW_BOX(self->shapes),
                            tile_new(clawt_face_shape_name((gint)i), tile,
                                     clawt_face_shape_label((gint)i)));
    }

    {
        GtkWidget *disc = adw_avatar_new(PICKER_TILE_SIZE, "A a", TRUE);

        gtk_flow_box_append(GTK_FLOW_BOX(self->shapes),
                            tile_new(CLAWT_FACE_INITIALS_NAME, disc,
                                     "Initials: the letter disc"));
    }
    gtk_box_append(GTK_BOX(self), self->shapes);

    /* ── Colours: Automatic, the palette, and a custom one ── */
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(row, GTK_ALIGN_CENTER);

    self->swatches = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(self->swatches),
                                    GTK_SELECTION_SINGLE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(self->swatches), TRUE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(self->swatches), 7);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(self->swatches), 13);
    gtk_flow_box_set_activate_on_single_click(GTK_FLOW_BOX(self->swatches),
                                              TRUE);
    gtk_flow_box_append(GTK_FLOW_BOX(self->swatches),
                        swatch_new(self, "",
                                   "Automatic: a tone picked from the id"));
    for (i = 0; i < CLAWT_FACE_PALETTE_SIZE; i++)
        gtk_flow_box_append(GTK_FLOW_BOX(self->swatches),
                            swatch_new(self, clawt_face_palette_color(i),
                                       clawt_face_palette_color(i)));
    gtk_box_append(GTK_BOX(row), self->swatches);

    self->custom = gtk_button_new_from_icon_name("list-add-symbolic");
    gtk_widget_add_css_class(self->custom, "circular");
    gtk_widget_add_css_class(self->custom, "flat");
    gtk_widget_set_valign(self->custom, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(self->custom, "Another colour");
    gtk_box_append(GTK_BOX(row), self->custom);
    gtk_box_append(GTK_BOX(self), row);

    g_signal_connect(self->shapes, "child-activated",
                     G_CALLBACK(on_shape_selected), self);
    g_signal_connect(self->swatches, "child-activated",
                     G_CALLBACK(on_swatch_selected), self);
    g_signal_connect(self->custom, "clicked",
                     G_CALLBACK(on_custom_clicked), self);

    /* the starting choice, without a "changed" nobody asked for */
    self->updating = TRUE;
    g_free(self->face);
    self->face = normalise_face(face);
    select_by(self->shapes, "face-value", self->face);
    g_free(self->color);
    self->color = g_strdup(clawt_color_ink(color) != NULL ? color : "");
    select_by(self->swatches, "color-value", self->color);
    self->updating = FALSE;
    picker_refresh(self);

    return GTK_WIDGET(self);
}
