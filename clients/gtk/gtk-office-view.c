/*
 * gtk-office-view.c - The drawn office, embedded in the Office page
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The office is drawn by clawtilla-office, on libregnum, and GTK never
 * links libregnum.  So the window runs it as a child: `clawtilla-office
 * --embed=3 <connection> --width W --height H`, fd 3 one end of a
 * socketpair.  The child renders offscreen and sends frames over it
 * (libregnum's frame protocol, spoken by ClawtOfficeFrames in libclawt);
 * this shows each one as a texture and sends the pointer back.  The
 * child's stdout is JSON lines -- `{"event":"open_agent","agent":ID}`
 * opens that agent's chat.
 *
 * The child runs only while it can be seen: the office is on and the
 * Office page is on screen (the slot is mapped -- a stack unmaps the
 * pages it is not showing), or the drawing is popped out into a window
 * of its own.  Leaving the page stops it; coming back starts a fresh
 * one.  A child that dies, or a stream that breaks, is a banner with
 * Restart in the slot, never anything the window has to survive.
 *
 * Size.  The drawing is the page's width and three quarters of the
 * page's visible height (clawt_office_view_height_for_page()), never
 * under CLAWT_OFFICE_VIEW_MIN_HEIGHT; popped out it is the whole window.
 * The child is asked for frames at that size in *device* pixels
 * (clawt_office_view_device_size()), so a frame covers the view pixel
 * for pixel, and a resize -- or a move to a monitor with another scale --
 * is a RESIZE message on the frame socket, never a restart.  Frames are
 * drawn by a paintable that scales with the nearest pixel, so the
 * moment between a resize and the frame drawn for it is blocky rather
 * than smeared, and pixel art is never filtered.
 *
 * What is drawn.  2D or 3D, Fit/1x/2x, whom the camera follows and the
 * device scale are one JSON line on the child's stdin
 * (clawt_office_view_control_line()), written whole whenever any of
 * them changes; the child answers on stdout with what it actually draws
 * ({"event":"view",...}: a world without a level stays 2D), and the
 * switch shows that.  The mode and zoom are remembered in
 * appearance.yaml (office_view, office_zoom); whom to follow is not,
 * because it names an agent of one fleet.
 */

#include "clawt-window-private.h"

#include <glib-unix.h>

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <unistd.h>

/* the popped-out window's first size, logical pixels */
#define POPOUT_WIDTH (1280)
#define POPOUT_HEIGHT (800)
/* resizes are sent once the size has held this long */
#define RESIZE_DEBOUNCE_MS (200)
/* a child asked to stop gets this long before it is killed */
#define STOP_GRACE_MS (3000)
/* the fd the child is told to use */
#define CHILD_FD (3)
/* how long a dead child's stderr has to arrive before its exit is reported */
#define EXIT_SETTLE_MS (300)
/* how long a closed frame socket waits for the child's own exit to explain it */
#define CLOSED_SETTLE_MS (1000)
/* the longest stdout or stderr line kept; the rest of it is dropped */
#define MAX_LINE (65536)

/* ── A frame, drawn nearest-neighbour ─────────────────────────────── */

/*
 * GtkPicture draws a texture with a linear filter, which smears pixel art
 * whenever the frame and the view are not the same size: between a
 * resize and the frame drawn for it, or a frame clamped to the
 * renderer's largest size.  This paintable draws its texture with
 * GSK_SCALING_FILTER_NEAREST instead, and is otherwise just the texture.
 */
#define CLAWT_TYPE_NEAREST_PAINTABLE (clawt_nearest_paintable_get_type())
G_DECLARE_FINAL_TYPE(ClawtNearestPaintable, clawt_nearest_paintable, CLAWT,
                     NEAREST_PAINTABLE, GObject)

struct _ClawtNearestPaintable {
    GObject     parent_instance;
    GdkTexture *texture;            /* the newest frame, or NULL */
};

static void clawt_nearest_paintable_iface_init(GdkPaintableInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(ClawtNearestPaintable, clawt_nearest_paintable,
                              G_TYPE_OBJECT,
                              G_IMPLEMENT_INTERFACE(GDK_TYPE_PAINTABLE,
                                                    clawt_nearest_paintable_iface_init))

static void
nearest_snapshot(GdkPaintable *paintable, GdkSnapshot *snapshot, gdouble width,
                 gdouble height)
{
    ClawtNearestPaintable *self = CLAWT_NEAREST_PAINTABLE(paintable);
    graphene_rect_t bounds;

    if (self->texture == NULL)
        return;
    graphene_rect_init(&bounds, 0.0f, 0.0f, (gfloat)width, (gfloat)height);
    gtk_snapshot_append_scaled_texture(GTK_SNAPSHOT(snapshot), self->texture,
                                       GSK_SCALING_FILTER_NEAREST, &bounds);
}

static gint
nearest_intrinsic_width(GdkPaintable *paintable)
{
    ClawtNearestPaintable *self = CLAWT_NEAREST_PAINTABLE(paintable);

    return self->texture != NULL ? gdk_texture_get_width(self->texture) : 0;
}

static gint
nearest_intrinsic_height(GdkPaintable *paintable)
{
    ClawtNearestPaintable *self = CLAWT_NEAREST_PAINTABLE(paintable);

    return self->texture != NULL ? gdk_texture_get_height(self->texture) : 0;
}

static void
clawt_nearest_paintable_iface_init(GdkPaintableInterface *iface)
{
    iface->snapshot = nearest_snapshot;
    iface->get_intrinsic_width = nearest_intrinsic_width;
    iface->get_intrinsic_height = nearest_intrinsic_height;
}

static void
clawt_nearest_paintable_finalize(GObject *object)
{
    ClawtNearestPaintable *self = CLAWT_NEAREST_PAINTABLE(object);

    g_clear_object(&self->texture);
    G_OBJECT_CLASS(clawt_nearest_paintable_parent_class)->finalize(object);
}

static void
clawt_nearest_paintable_class_init(ClawtNearestPaintableClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = clawt_nearest_paintable_finalize;
}

static void
clawt_nearest_paintable_init(ClawtNearestPaintable *self)
{
    (void)self;
}

/*
 * The frame to draw, or NULL for none.  Contents are invalidated every
 * frame; the size only when it changed, which is what makes the picture
 * lay itself out again.
 */
static void
clawt_nearest_paintable_set_texture(ClawtNearestPaintable *self, GdkTexture *texture)
{
    gboolean resized;

    resized = (self->texture == NULL) != (texture == NULL) ||
              (texture != NULL &&
               (gdk_texture_get_width(texture) != gdk_texture_get_width(self->texture) ||
                gdk_texture_get_height(texture) != gdk_texture_get_height(self->texture)));
    g_set_object(&self->texture, texture);
    if (resized)
        gdk_paintable_invalidate_size(GDK_PAINTABLE(self));
    gdk_paintable_invalidate_contents(GDK_PAINTABLE(self));
}

typedef struct _OfficeView OfficeView;

struct _OfficeView {
    ClawtWindow       *window;      /* not a reference; NULL once the window goes */
    GMainContext      *context;     /* the window's: every source goes here */

    GtkWidget         *banner;      /* AdwBanner: "stopped drawing", Restart */
    GtkWidget         *note;        /* missing binary, unfollowable connection */
    GtkWidget         *drawing;     /* the toolbar and the stage: what pops out */
    GtkWidget         *stage;       /* GtkOverlay: the size, and the picture on it */
    GtkWidget         *sizer;       /* the overlay's main child, whose size is the view's */
    GtkWidget         *picture;
    ClawtNearestPaintable *paintable;  /* the picture's, for as long as the view lives */

    /* the toolbar over the drawing */
    GtkWidget         *mode_2d;     /* GtkToggleButton, grouped with mode_3d */
    GtkWidget         *mode_3d;
    GtkWidget         *zoom_buttons[3];  /* Fit, 1x, 2x, by ClawtOfficeZoom */
    GtkWidget         *follow;      /* GtkDropDown: nobody, then the roster */
    GtkStringList     *follow_names;
    GPtrArray         *follow_ids;  /* parallel to follow_names; "" for nobody */
    GtkWidget         *popout_button;
    gboolean           syncing;     /* the toolbar is being set, not clicked */

    /* the drawing in a window of its own, and what the slot says meanwhile */
    GtkWidget         *popout;      /* AdwWindow, or NULL */
    GtkWidget         *toolbar_view;
    GtkWidget         *placeholder;

    /* the page's vertical adjustment: its page-size is the visible height */
    GtkAdjustment     *page_adjustment;
    gulong             page_handler;

    gboolean           enabled;     /* the office is on, per the last snapshot */
    gboolean           mapped;      /* the Office page is on screen */

    /* how it is to be drawn: what the control line says */
    gboolean           prefs_read;  /* mode and zoom taken from appearance.yaml */
    ClawtOfficeViewMode mode;
    ClawtOfficeZoom    zoom;
    gchar             *focus;       /* whom the camera follows, or NULL */
    gboolean           has_3d;      /* the child's last word on it; TRUE until told */

    /* one running child, and everything that belongs to it */
    guint              generation;  /* bumped on every stop: stale callbacks see it */
    GSubprocess       *child;
    GCancellable      *cancellable;
    ClawtOfficeFrames *frames;
    GSource           *read_source;
    GSource           *write_source;
    GSource           *resize_source;
    gchar             *connection_key;  /* which daemon the child was told about */
    gchar             *last_stderr;
    guint              frame_width;
    guint              frame_height;
    gint               want_width;      /* the size the next resize sends */
    gint               want_height;
    GOutputStream     *control;         /* the child's stdin, for control lines */
    gboolean           writing;         /* a control line is on its way */
    gboolean           dirty;           /* and the state changed again meanwhile */
};

/* What an async read or wait holds: the view, and which child it was for. */
typedef struct {
    OfficeView *view;               /* a reference */
    guint       generation;
} Ticket;

static void stop_child(OfficeView *view, const gchar *reason);
static void start_child(OfficeView *view);
static void send_control(OfficeView *view);
static void sync_toolbar(OfficeView *view);
static gboolean report_closed(gpointer user_data);
static void closed_soon(OfficeView *view);

/* ── Lifetime ─────────────────────────────────────────────────────── */

static void
view_clear(gpointer data)
{
    OfficeView *view = data;

    g_clear_pointer(&view->context, g_main_context_unref);
    g_clear_pointer(&view->connection_key, g_free);
    g_clear_pointer(&view->last_stderr, g_free);
    g_clear_pointer(&view->focus, g_free);
    g_clear_pointer(&view->follow_ids, g_ptr_array_unref);
    g_clear_object(&view->paintable);
}

static OfficeView *
view_ref(OfficeView *view)
{
    return g_rc_box_acquire(view);
}

static void
view_unref(OfficeView *view)
{
    g_rc_box_release_full(view, view_clear);
}

static Ticket *
ticket_new(OfficeView *view)
{
    Ticket *ticket = g_new0(Ticket, 1);

    ticket->view = view_ref(view);
    ticket->generation = view->generation;
    return ticket;
}

static void
ticket_free(Ticket *ticket)
{
    view_unref(ticket->view);
    g_free(ticket);
}

/* Whether a callback still describes the child that is running. */
static gboolean
ticket_current(Ticket *ticket)
{
    return ticket->view->window != NULL && ticket->view->child != NULL &&
           ticket->generation == ticket->view->generation;
}

static GSource *
attach_timeout(OfficeView *view, guint ms, GSourceFunc func, gpointer data,
               GDestroyNotify notify)
{
    GSource *source = g_timeout_source_new(ms);

    g_source_set_callback(source, func, data, notify);
    g_source_attach(source, view->context);
    return source;
}

static void
drop_source(GSource **source)
{
    if (*source != NULL) {
        g_source_destroy(*source);
        g_source_unref(*source);
        *source = NULL;
    }
}

/* ── What the slot says ───────────────────────────────────────────── */

static void
show_note(OfficeView *view, const gchar *text)
{
    gtk_label_set_text(GTK_LABEL(view->note), text != NULL ? text : "");
    gtk_widget_set_visible(view->note, text != NULL);
}

static void
show_banner(OfficeView *view, const gchar *reason)
{
    if (reason == NULL) {
        adw_banner_set_revealed(ADW_BANNER(view->banner), FALSE);
        return;
    }
    {
        g_autofree gchar *title = g_strdup_printf("The office stopped drawing: %s",
                                                  reason);

        adw_banner_set_title(ADW_BANNER(view->banner), title);
        adw_banner_set_revealed(ADW_BANNER(view->banner), TRUE);
    }
}

/* ── Frames in ────────────────────────────────────────────────────── */

static void
show_frame(OfficeView *view)
{
    guint width = 0;
    guint height = 0;
    guint stride = 0;
    g_autoptr(GBytes) pixels = clawt_office_frames_take_frame(view->frames, &width,
                                                              &height, &stride, NULL);
    g_autoptr(GdkTexture) texture = NULL;

    if (pixels == NULL)
        return;
    /* straight alpha, rows top to bottom: GDK's non-premultiplied RGBA */
    texture = gdk_memory_texture_new((gint)width, (gint)height, GDK_MEMORY_R8G8B8A8,
                                     pixels, stride);
    view->frame_width = width;
    view->frame_height = height;
    clawt_nearest_paintable_set_texture(view->paintable, texture);
}

static gboolean on_writable(gint fd, GIOCondition condition, gpointer user_data);

/*
 * Input the socket would not take waits for it to become writable --
 * watched only while there is some, so an idle office costs no wakeups.
 */
static void
sync_write_watch(OfficeView *view)
{
    gboolean pending = view->frames != NULL &&
                       clawt_office_frames_has_pending_output(view->frames);

    if (pending && view->write_source == NULL) {
        view->write_source = g_unix_fd_source_new(clawt_office_frames_get_fd(view->frames),
                                                  G_IO_OUT | G_IO_ERR | G_IO_HUP);
        g_source_set_callback(view->write_source, G_SOURCE_FUNC(on_writable), view, NULL);
        g_source_attach(view->write_source, view->context);
    } else if (!pending) {
        drop_source(&view->write_source);
    }
}

static gboolean
on_writable(gint fd, GIOCondition condition, gpointer user_data)
{
    OfficeView *view = user_data;
    g_autoptr(GError) error = NULL;

    (void)fd;
    (void)condition;
    if (!clawt_office_frames_flush(view->frames, &error)) {
        if (g_error_matches(error, CLAWT_ERROR, CLAWT_ERROR_NOT_CONNECTED))
            closed_soon(view);
        else
            stop_child(view, error->message);
        return G_SOURCE_REMOVE;
    }
    if (clawt_office_frames_has_pending_output(view->frames))
        return G_SOURCE_CONTINUE;
    /* removed here, so the pointer must not be destroyed a second time */
    g_clear_pointer(&view->write_source, g_source_unref);
    return G_SOURCE_REMOVE;
}

static gboolean
on_readable(gint fd, GIOCondition condition, gpointer user_data)
{
    OfficeView *view = user_data;
    g_autoptr(GError) error = NULL;

    (void)fd;
    (void)condition;
    if (!clawt_office_frames_dispatch(view->frames, &error)) {
        /*
         * A closed socket is usually a child on its way out, and its exit
         * status says more than this does: wait for it, briefly.
         */
        if (g_error_matches(error, CLAWT_ERROR, CLAWT_ERROR_NOT_CONNECTED)) {
            closed_soon(view);
            return G_SOURCE_REMOVE;
        }
        stop_child(view, error->message);
        return G_SOURCE_REMOVE;
    }
    show_frame(view);
    sync_write_watch(view);
    return G_SOURCE_CONTINUE;
}

/* ── Input out ────────────────────────────────────────────────────── */

/*
 * A socket the child closed: usually a child on its way out, whose exit
 * status says more than the socket does, so the report waits for it --
 * briefly -- rather than saying "closed" first.
 */
static void
closed_soon(OfficeView *view)
{
    drop_source(&view->read_source);
    drop_source(&view->write_source);
    g_source_unref(attach_timeout(view, CLOSED_SETTLE_MS, report_closed, ticket_new(view),
                                  (GDestroyNotify)ticket_free));
}

/*
 * After a send.  @error is the location the send wrote to, taken by
 * address: passing the GError itself beside the call that sets it reads
 * it before the call, since argument order is unspecified.  A send that
 * fails ends the child -- the stream cannot be trusted after it.
 */
static void
input_sent(OfficeView *view, gboolean ok, GError **error)
{
    if (ok) {
        sync_write_watch(view);
        return;
    }
    if (*error != NULL && g_error_matches(*error, CLAWT_ERROR, CLAWT_ERROR_NOT_CONNECTED)) {
        closed_soon(view);
        return;
    }
    stop_child(view, *error != NULL ? (*error)->message : "could not send input");
}

/*
 * A point on the picture, in frame pixels.  The frame is drawn
 * contained -- scaled to fit, centred -- so the letterbox is subtracted
 * before scaling, and a point in it is FALSE.
 */
static gboolean
to_frame(OfficeView *view, gdouble x, gdouble y, gfloat *fx, gfloat *fy)
{
    gdouble width = gtk_widget_get_width(view->picture);
    gdouble height = gtk_widget_get_height(view->picture);
    gdouble scale;
    gdouble ox;
    gdouble oy;

    if (view->frame_width == 0 || view->frame_height == 0 || width <= 0 || height <= 0)
        return FALSE;
    scale = MIN(width / view->frame_width, height / view->frame_height);
    ox = (width - view->frame_width * scale) / 2.0;
    oy = (height - view->frame_height * scale) / 2.0;
    *fx = (gfloat)((x - ox) / scale);
    *fy = (gfloat)((y - oy) / scale);
    return *fx >= 0 && *fy >= 0 && *fx < view->frame_width && *fy < view->frame_height;
}

static OfficeView *
running_view(ClawtWindow *self)
{
    OfficeView *view = self->office_view;

    return view != NULL && view->frames != NULL ? view : NULL;
}

static void
on_motion(GtkEventControllerMotion *controller, gdouble x, gdouble y,
          gpointer user_data)
{
    OfficeView *view = running_view(user_data);
    g_autoptr(GError) error = NULL;
    gfloat fx;
    gfloat fy;

    (void)controller;
    if (view == NULL || !to_frame(view, x, y, &fx, &fy))
        return;
    input_sent(view, clawt_office_frames_send_pointer_move(view->frames, fx, fy, &error),
               &error);
}

/* GDK numbers buttons 1 left, 2 middle, 3 right; raylib 0, 2, 1. */
static gint
frame_button(guint gdk_button)
{
    switch (gdk_button) {
    case GDK_BUTTON_PRIMARY:   return 0;
    case GDK_BUTTON_SECONDARY: return 1;
    case GDK_BUTTON_MIDDLE:    return 2;
    default:                   return -1;
    }
}

static void
send_click(GtkGestureClick *gesture, gdouble x, gdouble y, gboolean pressed,
           gpointer user_data)
{
    OfficeView *view = running_view(user_data);
    gint button = frame_button(gtk_gesture_single_get_current_button(
        GTK_GESTURE_SINGLE(gesture)));
    g_autoptr(GError) error = NULL;
    gfloat fx;
    gfloat fy;

    if (view == NULL || button < 0 || !to_frame(view, x, y, &fx, &fy))
        return;
    input_sent(view, clawt_office_frames_send_button(view->frames, (guint)button, pressed,
                                                     fx, fy, &error), &error);
}

static void
on_pressed(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y,
           gpointer user_data)
{
    (void)n_press;
    send_click(gesture, x, y, TRUE, user_data);
}

static void
on_released(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y,
            gpointer user_data)
{
    (void)n_press;
    send_click(gesture, x, y, FALSE, user_data);
}

/* GDK's positive dy is toward the user; the protocol's is away. */
static gboolean
on_scroll(GtkEventControllerScroll *controller, gdouble dx, gdouble dy,
          gpointer user_data)
{
    OfficeView *view = running_view(user_data);
    g_autoptr(GError) error = NULL;

    (void)controller;
    if (view == NULL)
        return FALSE;
    input_sent(view, clawt_office_frames_send_scroll(view->frames, (gfloat)dx,
                                                     (gfloat)-dy, &error), &error);
    return TRUE;
}

/* ── Size ─────────────────────────────────────────────────────────── */

static gboolean
send_resize(gpointer user_data)
{
    OfficeView *view = user_data;
    g_autoptr(GError) error = NULL;

    g_clear_pointer(&view->resize_source, g_source_unref);
    if (view->frames == NULL || view->want_width <= 0 || view->want_height <= 0)
        return G_SOURCE_REMOVE;
    if (view->frame_width == (guint)view->want_width &&
        view->frame_height == (guint)view->want_height)
        return G_SOURCE_REMOVE;
    input_sent(view, clawt_office_frames_send_resize(view->frames, (guint)view->want_width,
                                                     (guint)view->want_height, &error),
               &error);
    return G_SOURCE_REMOVE;
}

/*
 * The size the child is asked for: the sizer's, in device pixels.  Before
 * the sizer has been laid out, the fallback; a size the renderer cannot
 * draw at is clamped by clawt_office_view_device_size().
 */
static void
want_size(OfficeView *view, gint width, gint height)
{
    gint scale = gtk_widget_get_scale_factor(view->sizer);
    guint w = 0;
    guint h = 0;

    if (width <= 0 || height <= 0) {
        width = CLAWT_OFFICE_VIEW_FALLBACK_WIDTH;
        height = CLAWT_OFFICE_VIEW_FALLBACK_HEIGHT;
    }
    if (!clawt_office_view_device_size(width, height, scale, &w, &h))
        return;
    view->want_width = (gint)w;
    view->want_height = (gint)h;
}

/* A resize, once the size has held for RESIZE_DEBOUNCE_MS. */
static void
resize_soon(OfficeView *view)
{
    if (view->frames == NULL)
        return;
    drop_source(&view->resize_source);
    view->resize_source = attach_timeout(view, RESIZE_DEBOUNCE_MS, send_resize, view,
                                         NULL);
}

/*
 * The sizer is the overlay's main child, so its allocation is exactly
 * the area the picture covers.  A window being dragged wider resizes it
 * every frame; the child gets the size it settles on.
 */
static void
on_sizer_resize(GtkDrawingArea *area, gint width, gint height, gpointer user_data)
{
    ClawtWindow *self = user_data;
    OfficeView *view = self->office_view;

    (void)area;
    if (view == NULL)
        return;
    want_size(view, width, height);
    resize_soon(view);
}

/*
 * A move to a monitor with another scale: the same logical size is a
 * different number of device pixels, and text in the frame is drawn at
 * the new scale.  "resize" is not emitted for this -- the logical size
 * did not change -- so it is watched for itself.
 */
static void
on_scale_factor(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    ClawtWindow *self = user_data;
    OfficeView *view = self->office_view;

    (void)object;
    (void)pspec;
    if (view == NULL)
        return;
    want_size(view, gtk_widget_get_width(view->sizer), gtk_widget_get_height(view->sizer));
    resize_soon(view);
    send_control(view);
}

/*
 * The drawing's height on the page, from the page's visible height.  The
 * page's vertical adjustment's page-size is exactly that, and it is
 * notified when the window is resized, maximized or tiled -- and not when
 * the content changes, so setting the sizer's request from it cannot
 * feed back into it.
 */
static void
apply_page_height(OfficeView *view)
{
    gint page = view->page_adjustment != NULL
        ? (gint)gtk_adjustment_get_page_size(view->page_adjustment) : 0;

    if (view->popout != NULL)
        return;
    gtk_widget_set_size_request(view->sizer, -1, clawt_office_view_height_for_page(page));
}

static void
on_page_size(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    OfficeView *view = user_data;

    (void)object;
    (void)pspec;
    apply_page_height(view);
}

/* The scrolled page the slot is in, found once it has been put in one. */
static void
watch_page(OfficeView *view)
{
    GtkWidget *scrolled;

    if (view->page_adjustment != NULL || view->window == NULL)
        return;
    scrolled = gtk_widget_get_ancestor(view->window->office_view_slot,
                                       GTK_TYPE_SCROLLED_WINDOW);
    if (scrolled == NULL)
        return;
    view->page_adjustment = g_object_ref(
        gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrolled)));
    view->page_handler = g_signal_connect(view->page_adjustment, "notify::page-size",
                                          G_CALLBACK(on_page_size), view);
    apply_page_height(view);
}

static void
unwatch_page(OfficeView *view)
{
    if (view->page_adjustment == NULL)
        return;
    g_clear_signal_handler(&view->page_handler, view->page_adjustment);
    g_clear_object(&view->page_adjustment);
}

/* ── Control lines out ────────────────────────────────────────────── */

typedef struct {
    Ticket *ticket;
    gchar  *line;                   /* the buffer the write reads from */
} ControlWrite;

static void
control_write_free(ControlWrite *write)
{
    ticket_free(write->ticket);
    g_free(write->line);
    g_free(write);
}

static void
on_control_written(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ControlWrite *write = user_data;
    OfficeView *view = write->ticket->view;
    g_autoptr(GError) error = NULL;
    gboolean current = ticket_current(write->ticket);

    /*
     * A failed write is a child on its way out, whose exit says why;
     * reporting it here too would say the same thing twice, worse.
     */
    if (!g_output_stream_write_all_finish(G_OUTPUT_STREAM(source), result, NULL, &error) &&
        !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_debug("clawtilla-office control: %s", error->message);
    /* a stale write belongs to a child that is gone: the flags are the new one's */
    if (current) {
        view->writing = FALSE;
        if (view->dirty) {
            view->dirty = FALSE;
            send_control(view);
        }
    }
    control_write_free(write);
}

/*
 * Tell the child how to draw: the whole state, one line.  One write at a
 * time on the stream, and a change during it is coalesced into the next
 * -- which says the newest state, so nothing in between is worth
 * queueing.
 */
static void
send_control(OfficeView *view)
{
    ControlWrite *write;
    gint scale;

    if (view->control == NULL || view->child == NULL || view->window == NULL)
        return;
    if (view->writing) {
        view->dirty = TRUE;
        return;
    }
    scale = gtk_widget_get_scale_factor(view->sizer);
    write = g_new0(ControlWrite, 1);
    write->ticket = ticket_new(view);
    write->line = clawt_office_view_control_line(view->mode, view->focus, view->zoom, scale);
    view->writing = TRUE;

    g_main_context_push_thread_default(view->context);
    g_output_stream_write_all_async(view->control, write->line, strlen(write->line),
                                    G_PRIORITY_DEFAULT, view->cancellable,
                                    on_control_written, write);
    g_main_context_pop_thread_default(view->context);
}

/* ── The child's stdout and stderr ────────────────────────────────── */

/*
 * A line reader that keeps at most MAX_LINE of a line.
 * g_data_input_stream_read_line_async() grows its buffer until it finds a
 * newline, so a child printing without one would grow it without end.
 */
typedef struct {
    Ticket       *ticket;
    GInputStream *stream;
    GString      *line;
    gboolean      overflow;
    gboolean      is_stdout;
    guint8        buf[4096];
} LineReader;

static void
line_reader_free(LineReader *reader)
{
    ticket_free(reader->ticket);
    g_object_unref(reader->stream);
    g_string_free(reader->line, TRUE);
    g_free(reader);
}

typedef struct {
    Ticket *ticket;
    gchar  *agent;
} OpenAgent;

static void
open_agent_free(gpointer data)
{
    OpenAgent *open = data;

    ticket_free(open->ticket);
    g_free(open->agent);
    g_free(open);
}

static gboolean
open_agent_later(gpointer user_data)
{
    OpenAgent *open = user_data;
    ClawtWindow *self = open->ticket->view->window;

    if (self != NULL) {
        clawt_gtk_select_agent(self, open->agent);
        clawt_gtk_show_page(self, CLAWT_PAGE_CHAT);
    }
    return G_SOURCE_REMOVE;
}

/* One line of the child's stdout: a JSON event, or nothing we know. */
static void
handle_event(OfficeView *view, const gchar *line)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    JsonNode *root;
    JsonObject *object;
    const gchar *agent;

    if (!json_parser_load_from_data(parser, line, -1, NULL))
        return;
    root = json_parser_get_root(parser);
    if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root) ||
        (object = json_node_get_object(root)) == NULL)
        return;

    /*
     * What the child draws: the switch shows it, and 3D is offered only
     * when the world has a level.  view->mode -- what was asked for, and
     * remembered -- is left alone, so the next world with a level is
     * drawn the way the person chose.
     */
    if (g_strcmp0(clawt_json_string(object, "event", NULL), "view") == 0) {
        view->has_3d = clawt_json_boolean(object, "has_3d", TRUE);
        sync_toolbar(view);
        view->syncing = TRUE;
        gtk_toggle_button_set_active(
            GTK_TOGGLE_BUTTON(g_strcmp0(clawt_json_string(object, "mode", "2d"), "3d") == 0
                              ? view->mode_3d : view->mode_2d), TRUE);
        view->syncing = FALSE;
        return;
    }
    if (g_strcmp0(clawt_json_string(object, "event", NULL), "open_agent") != 0)
        return;
    agent = clawt_json_string(object, "agent", NULL);
    if (agent == NULL || *agent == '\0')
        return;

    /*
     * From an idle: opening the chat leaves the Office page, which stops
     * this child, which cancels the read this line came from.
     */
    {
        OpenAgent *open = g_new0(OpenAgent, 1);

        open->ticket = ticket_new(view);
        open->agent = g_strdup(agent);
        g_source_unref(attach_timeout(view, 0, open_agent_later, open, open_agent_free));
    }
}

static void
handle_line(LineReader *reader)
{
    OfficeView *view = reader->ticket->view;
    const gchar *line = reader->line->str;

    if (reader->is_stdout) {
        handle_event(view, line);
        return;
    }
    if (*g_strstrip(reader->line->str) == '\0')
        return;
    g_debug("clawtilla-office: %s", line);
    g_free(view->last_stderr);
    view->last_stderr = g_strdup(line);
}

static void line_reader_next(LineReader *reader);

static void
on_line_read(GObject *source, GAsyncResult *result, gpointer user_data)
{
    LineReader *reader = user_data;
    g_autoptr(GError) error = NULL;
    gssize n = g_input_stream_read_finish(G_INPUT_STREAM(source), result, &error);
    gssize i;

    if (n <= 0 || !ticket_current(reader->ticket)) {
        line_reader_free(reader);
        return;
    }
    for (i = 0; i < n; i++) {
        gchar c = (gchar)reader->buf[i];

        if (c == '\n') {
            if (!reader->overflow)
                handle_line(reader);
            g_string_truncate(reader->line, 0);
            reader->overflow = FALSE;
        } else if (reader->line->len < MAX_LINE) {
            g_string_append_c(reader->line, c);
        } else {
            reader->overflow = TRUE;
        }
    }
    line_reader_next(reader);
}

static void
line_reader_next(LineReader *reader)
{
    OfficeView *view = reader->ticket->view;

    /* an async read attaches to the thread-default context: name ours */
    g_main_context_push_thread_default(view->context);
    g_input_stream_read_async(reader->stream, reader->buf, sizeof(reader->buf),
                              G_PRIORITY_DEFAULT, view->cancellable, on_line_read, reader);
    g_main_context_pop_thread_default(view->context);
}

static void
read_lines(OfficeView *view, GInputStream *stream, gboolean is_stdout)
{
    LineReader *reader = g_new0(LineReader, 1);

    reader->ticket = ticket_new(view);
    reader->stream = g_object_ref(stream);
    reader->line = g_string_new(NULL);
    reader->is_stdout = is_stdout;
    line_reader_next(reader);
}

/* ── Starting and stopping the child ──────────────────────────────── */

/* "exited with status 1: <its last line on stderr>" */
static gchar *
exit_reason(OfficeView *view, GSubprocess *child)
{
    g_autofree gchar *how = NULL;

    /* a signal is its own reason: the last line printed is unrelated chatter */
    if (g_subprocess_get_if_signaled(child))
        return g_strdup_printf("clawtilla-office was killed by signal %d (%s)",
                               g_subprocess_get_term_sig(child),
                               g_strsignal(g_subprocess_get_term_sig(child)));
    how = g_strdup_printf("clawtilla-office exited with status %d",
                          g_subprocess_get_exit_status(child));
    if (view->last_stderr == NULL)
        return g_steal_pointer(&how);
    return g_strdup_printf("%s: %s", how, view->last_stderr);
}

/*
 * Reported a moment after the exit, not at it: the child's last words on
 * stderr are usually the reason, and the exit can be seen before the
 * read that carries them.
 */
static gboolean
report_exit(gpointer user_data)
{
    Ticket *ticket = user_data;
    g_autofree gchar *reason = NULL;

    if (ticket_current(ticket)) {
        reason = exit_reason(ticket->view, ticket->view->child);
        stop_child(ticket->view, reason);
    }
    return G_SOURCE_REMOVE;
}

static void
on_child_exited(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Ticket *ticket = user_data;

    g_subprocess_wait_finish(G_SUBPROCESS(source), result, NULL);
    if (ticket_current(ticket))
        g_source_unref(attach_timeout(ticket->view, EXIT_SETTLE_MS, report_exit, ticket,
                                      (GDestroyNotify)ticket_free));
    else
        ticket_free(ticket);
}

/* The socket closed and the child has not exited to say why. */
static gboolean
report_closed(gpointer user_data)
{
    Ticket *ticket = user_data;
    g_autofree gchar *reason = NULL;

    if (ticket_current(ticket)) {
        reason = g_strdup_printf("clawtilla-office closed its frame socket%s%s",
                                 ticket->view->last_stderr != NULL ? ": " : "",
                                 ticket->view->last_stderr != NULL
                                     ? ticket->view->last_stderr : "");
        stop_child(ticket->view, reason);
    }
    return G_SOURCE_REMOVE;
}

typedef struct {
    pid_t ppid;
} ChildSetup;

/*
 * In the child, before exec: die with the window.  Only the parent
 * *thread* dying sends the signal, and this is forked from the main
 * thread, which is the window's.  A parent that died before the prctl
 * would never send it, hence the check after.
 */
static void
child_setup(gpointer user_data)
{
    ChildSetup *setup = user_data;

    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() != setup->ppid)
        _exit(1);
}

/* Which daemon the window is on, as the child would be told it. */
static gchar *
connection_key(ClawtWindow *self)
{
    ClawtConnection *c = self->active_connection;

    if (c == NULL)
        return g_strdup("");
    if (clawt_connection_is_local(c))
        return g_strdup_printf("local:%s", clawt_connection_get_socket_path(c) != NULL
                                           ? clawt_connection_get_socket_path(c) : "");
    return g_strdup_printf("remote:%s", clawt_connection_get_name(c));
}

static gboolean
connection_saved(ClawtWindow *self)
{
    guint i;

    if (self->active_connection == NULL || self->connections == NULL)
        return FALSE;
    for (i = 0; i < self->connections->len; i++) {
        ClawtConnection *c = g_ptr_array_index(self->connections, i);

        if (g_strcmp0(clawt_connection_get_name(c),
                      clawt_connection_get_name(self->active_connection)) == 0)
            return TRUE;
    }
    return FALSE;
}

static void
start_child(OfficeView *view)
{
    ClawtWindow *self = view->window;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *binary = NULL;
    g_auto(GStrv) argv = NULL;
    g_autoptr(GSubprocessLauncher) launcher = NULL;
    static ChildSetup setup;
    gint sv[2];

    if (self == NULL || view->child != NULL || !view->enabled ||
        (!view->mapped && view->popout == NULL))
        return;

    show_banner(view, NULL);
    binary = clawt_office_renderer_resolve(&error);
    if (binary == NULL) {
        show_note(view, error->message);
        gtk_widget_set_visible(view->stage, FALSE);
        return;
    }

    want_size(view, gtk_widget_get_width(view->sizer), gtk_widget_get_height(view->sizer));
    argv = clawt_office_renderer_embed_argv(binary, CHILD_FD, self->active_connection,
                                            connection_saved(self),
                                            (guint)view->want_width,
                                            (guint)view->want_height, &error);
    if (argv == NULL) {
        show_note(view, error->message);
        gtk_widget_set_visible(view->stage, FALSE);
        return;
    }

    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
        g_autofree gchar *reason = g_strdup_printf("socketpair: %s", g_strerror(errno));

        show_note(view, NULL);
        show_banner(view, reason);
        return;
    }

    /* stdin carries control lines: the mode, zoom, follow and scale */
    launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDIN_PIPE |
                                         G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                         G_SUBPROCESS_FLAGS_STDERR_PIPE);
    /* the launcher closes our copy of the child's end once it has spawned */
    g_subprocess_launcher_take_fd(launcher, sv[1], CHILD_FD);
    setup.ppid = getpid();
    g_subprocess_launcher_set_child_setup(launcher, child_setup, &setup, NULL);
    view->child = g_subprocess_launcher_spawnv(launcher, (const gchar * const *)argv,
                                               &error);
    if (view->child == NULL) {
        close(sv[0]);
        show_note(view, NULL);
        show_banner(view, error->message);
        return;
    }

    show_note(view, NULL);
    gtk_widget_set_visible(view->stage, TRUE);
    g_clear_pointer(&view->last_stderr, g_free);
    g_free(view->connection_key);
    view->connection_key = connection_key(self);
    view->cancellable = g_cancellable_new();
    view->frames = clawt_office_frames_new(sv[0], TRUE);
    view->frame_width = 0;
    view->frame_height = 0;

    view->read_source = g_unix_fd_source_new(sv[0], G_IO_IN | G_IO_HUP | G_IO_ERR);
    g_source_set_callback(view->read_source, G_SOURCE_FUNC(on_readable), view, NULL);
    g_source_attach(view->read_source, view->context);

    read_lines(view, g_subprocess_get_stdout_pipe(view->child), TRUE);
    read_lines(view, g_subprocess_get_stderr_pipe(view->child), FALSE);

    g_main_context_push_thread_default(view->context);
    g_subprocess_wait_async(view->child, view->cancellable, on_child_exited,
                            ticket_new(view));
    g_main_context_pop_thread_default(view->context);

    /* before anything is drawn: the world loads in the mode asked for */
    view->control = g_object_ref(g_subprocess_get_stdin_pipe(view->child));
    view->writing = FALSE;
    view->dirty = FALSE;
    view->has_3d = TRUE;
    sync_toolbar(view);
    send_control(view);
}

/* A child asked to stop, and killed if it has not within the grace. */
typedef struct {
    GSubprocess *child;
    GSource     *deadline;
} Reaper;

static gboolean
reaper_deadline(gpointer user_data)
{
    Reaper *reaper = user_data;

    g_subprocess_force_exit(reaper->child);
    g_clear_pointer(&reaper->deadline, g_source_unref);
    return G_SOURCE_REMOVE;
}

static void
reaper_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Reaper *reaper = user_data;

    g_subprocess_wait_finish(G_SUBPROCESS(source), result, NULL);
    drop_source(&reaper->deadline);
    g_object_unref(reaper->child);
    g_free(reaper);
}

static void
reap(OfficeView *view, GSubprocess *child)
{
    Reaper *reaper;

    /* already reaped: nothing to signal, and its pid may be somebody else's */
    if (g_subprocess_get_identifier(child) == NULL)
        return;
    g_subprocess_send_signal(child, SIGTERM);

    reaper = g_new0(Reaper, 1);
    reaper->child = g_object_ref(child);
    reaper->deadline = attach_timeout(view, STOP_GRACE_MS, reaper_deadline, reaper, NULL);
    g_main_context_push_thread_default(view->context);
    g_subprocess_wait_async(child, NULL, reaper_done, reaper);
    g_main_context_pop_thread_default(view->context);
}

/*
 * Everything the running child owns, gone: the reads cancelled, the
 * sources removed, the socket closed, the process asked to stop.  With
 * a reason the slot says why and offers Restart; without one this was
 * asked for, and says nothing.
 */
static void
stop_child(OfficeView *view, const gchar *reason)
{
    g_autofree gchar *why = g_strdup(reason);

    view->generation++;
    if (view->cancellable != NULL)
        g_cancellable_cancel(view->cancellable);
    g_clear_object(&view->cancellable);
    drop_source(&view->read_source);
    drop_source(&view->write_source);
    drop_source(&view->resize_source);
    g_clear_object(&view->frames);
    g_clear_object(&view->control);
    view->writing = FALSE;
    view->dirty = FALSE;
    if (view->child != NULL) {
        GSubprocess *child = g_steal_pointer(&view->child);

        reap(view, child);
        g_object_unref(child);
    }
    if (view->window != NULL) {
        clawt_nearest_paintable_set_texture(view->paintable, NULL);
        /* an empty frame under the banner reads as a renderer still loading */
        if (why != NULL)
            gtk_widget_set_visible(view->stage, FALSE);
        show_banner(view, why);
    }
}

/* Start, stop or restart, from what is now true. */
static void
reconsider(OfficeView *view)
{
    ClawtWindow *self = view->window;

    if (self == NULL)
        return;
    gtk_widget_set_visible(self->office_view_slot, view->enabled);

    /* the mode and zoom the person last chose, once the window has read them */
    if (!view->prefs_read && self->appearance != NULL) {
        view->prefs_read = TRUE;
        view->mode = clawt_appearance_get_office_mode(self->appearance);
        view->zoom = clawt_appearance_get_office_zoom(self->appearance);
        sync_toolbar(view);
    }
    /* an office turned off takes its popped-out window with it */
    if (!view->enabled && view->popout != NULL)
        gtk_window_close(GTK_WINDOW(view->popout));
    if (!view->enabled || (!view->mapped && view->popout == NULL)) {
        stop_child(view, NULL);
        return;
    }
    /* told about another daemon than the window is on now: start over */
    if (view->child != NULL) {
        g_autofree gchar *key = connection_key(self);

        if (g_strcmp0(key, view->connection_key) == 0)
            return;
        stop_child(view, NULL);
    }
    /* a stopped child stays stopped until Restart: never a respawn loop */
    if (adw_banner_get_revealed(ADW_BANNER(view->banner)))
        return;
    start_child(view);
}

static void
on_restart(AdwBanner *banner, gpointer user_data)
{
    ClawtWindow *self = user_data;
    OfficeView *view = self->office_view;

    (void)banner;
    if (view == NULL)
        return;
    stop_child(view, NULL);
    start_child(view);
}

static void
on_slot_map(GtkWidget *widget, gpointer user_data)
{
    ClawtWindow *self = user_data;

    (void)widget;
    if (self->office_view == NULL)
        return;
    ((OfficeView *)self->office_view)->mapped = TRUE;
    /* by now the slot is in its page, whose height sets the drawing's */
    watch_page(self->office_view);
    /* a fresh visit is a fresh try */
    show_banner(self->office_view, NULL);
    reconsider(self->office_view);
}

static void
on_slot_unmap(GtkWidget *widget, gpointer user_data)
{
    ClawtWindow *self = user_data;

    (void)widget;
    if (self->office_view == NULL)
        return;
    ((OfficeView *)self->office_view)->mapped = FALSE;
    reconsider(self->office_view);
}

/* ── The toolbar ──────────────────────────────────────────────────── */

/*
 * The toolbar from the view's state, without its handlers acting on it:
 * the asked-for mode and zoom, and 3D offered only while the world has a
 * level.  The 2D/3D toggle then follows what the child says it draws.
 */
static void
sync_toolbar(OfficeView *view)
{
    if (view->mode_2d == NULL)
        return;
    view->syncing = TRUE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(view->mode == CLAWT_OFFICE_VIEW_3D &&
                                                   view->has_3d
                                                   ? view->mode_3d : view->mode_2d), TRUE);
    gtk_widget_set_sensitive(view->mode_3d, view->has_3d);
    gtk_widget_set_tooltip_text(view->mode_3d, view->has_3d
        ? "Draw the 3D level, from an orbiting camera"
        : "This world has no 3D level; it is drawn in 2D");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(view->zoom_buttons[view->zoom]), TRUE);
    view->syncing = FALSE;
}

/* A choice the person made, kept in appearance.yaml for the next window. */
static void
save_prefs(OfficeView *view)
{
    ClawtWindow *self = view->window;
    g_autoptr(GError) error = NULL;

    if (self == NULL || self->appearance == NULL)
        return;
    if (clawt_appearance_get_office_mode(self->appearance) == view->mode &&
        clawt_appearance_get_office_zoom(self->appearance) == view->zoom)
        return;
    clawt_appearance_set_office_mode(self->appearance, view->mode);
    clawt_appearance_set_office_zoom(self->appearance, view->zoom);
    if (!clawt_appearance_save(self->appearance, NULL, &error))
        g_warning("could not save the office view: %s", error->message);
}

static void
on_mode_toggled(GtkToggleButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    OfficeView *view = self->office_view;
    ClawtOfficeViewMode mode;

    if (view == NULL || view->syncing || !gtk_toggle_button_get_active(button))
        return;
    mode = GTK_WIDGET(button) == view->mode_3d ? CLAWT_OFFICE_VIEW_3D : CLAWT_OFFICE_VIEW_2D;
    if (mode == view->mode)
        return;
    view->mode = mode;
    save_prefs(view);
    send_control(view);
}

/* ── Open office: clawtilla-office as a program of its own ────────── */

void
clawt_gtk_office_open_window(ClawtWindow *self, ClawtOfficeViewMode mode)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GSubprocess) child = NULL;
    g_autofree gchar *handed_over = NULL;
    g_autofree gchar *text = NULL;
    const gchar *mode_name = mode == CLAWT_OFFICE_VIEW_3D ? "3D" : "2D";

    g_return_if_fail(CLAWT_IS_WINDOW(self));

    /*
     * On this window's own connection, said outright, never the one the
     * renderer would follow by itself; a typed connection's token goes
     * through a pipe (clawt_office_renderer_open_window()).  Not waited
     * for: the window is the renderer's from here, and outlives this one.
     */
    child = clawt_office_renderer_open_window(self->active_connection,
                                              connection_saved(self), mode,
                                              &handed_over, &error);
    if (child == NULL) {
        text = g_strdup_printf("The office did not open: %s", error->message);
        g_message("%s", text);
        clawt_window_toast(self, text);
        return;
    }

    /* which daemon it was handed, for the log and the person; never the token */
    g_message("opened clawtilla-office (pid %s) in %s on %s",
              g_subprocess_get_identifier(child), mode_name, handed_over);
    text = g_strdup_printf("Opened the office in %s on %s.", mode_name, handed_over);
    clawt_window_toast(self, text);
}

/* The toolbar's Open office: in the mode the 2D/3D switch beside it shows. */
static void
on_open_office_clicked(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    OfficeView *view = self->office_view;

    (void)button;
    clawt_gtk_office_open_window(self, view != NULL ? view->mode : CLAWT_OFFICE_VIEW_2D);
}

void
clawt_gtk_on_open_office(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    const gchar *nick = parameter != NULL ? g_variant_get_string(parameter, NULL) : "2d";

    (void)action;
    clawt_gtk_office_open_window(CLAWT_WINDOW(user_data),
                                 g_strcmp0(nick, "3d") == 0 ? CLAWT_OFFICE_VIEW_3D
                                                            : CLAWT_OFFICE_VIEW_2D);
}

static void
on_zoom_toggled(GtkToggleButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    OfficeView *view = self->office_view;
    guint i;

    if (view == NULL || view->syncing || !gtk_toggle_button_get_active(button))
        return;
    for (i = 0; i < G_N_ELEMENTS(view->zoom_buttons); i++) {
        if (GTK_WIDGET(button) != view->zoom_buttons[i])
            continue;
        view->zoom = (ClawtOfficeZoom)i;
        save_prefs(view);
        send_control(view);
        return;
    }
}

static void
on_follow_selected(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    ClawtWindow *self = user_data;
    OfficeView *view = self->office_view;
    guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(object));
    const gchar *id;

    (void)pspec;
    if (view == NULL || view->syncing || view->follow_ids == NULL)
        return;
    id = selected < view->follow_ids->len ? g_ptr_array_index(view->follow_ids, selected) : "";
    g_free(view->focus);
    view->focus = *id != '\0' ? g_strdup(id) : NULL;
    send_control(view);
}

/* A group of linked toggle buttons, each in @group with the first. */
static GtkWidget *
toggle_in(GtkWidget *box, GtkWidget *first, const gchar *label, const gchar *tooltip)
{
    GtkWidget *button = gtk_toggle_button_new_with_label(label);

    gtk_widget_set_tooltip_text(button, tooltip);
    if (first != NULL)
        gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(button), GTK_TOGGLE_BUTTON(first));
    gtk_box_append(GTK_BOX(box), button);
    return button;
}

/* ── Popping out ──────────────────────────────────────────────────── */

/* The drawing moved from one parent to another, kept alive across it. */
static void
move_drawing(OfficeView *view, GtkWidget *to_slot_box)
{
    GtkWidget *drawing = g_object_ref(view->drawing);
    GtkWidget *parent = gtk_widget_get_parent(drawing);

    if (ADW_IS_TOOLBAR_VIEW(parent))
        adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(parent), NULL);
    else if (parent != NULL)
        gtk_box_remove(GTK_BOX(parent), drawing);
    if (to_slot_box != NULL)
        gtk_box_append(GTK_BOX(to_slot_box), drawing);
    else
        adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view->toolbar_view), drawing);
    g_object_unref(drawing);
}

/*
 * The popped-out window closing: the drawing goes back into the page,
 * sized from it again, and the child keeps running only if the page is
 * on screen.  Returning FALSE lets the window close.
 */
static gboolean
on_popout_close(GtkWindow *window, gpointer user_data)
{
    OfficeView *view = user_data;

    (void)window;
    if (view->popout == NULL)
        return FALSE;
    move_drawing(view, view->window != NULL ? view->window->office_view_slot : NULL);
    view->popout = NULL;
    view->toolbar_view = NULL;
    gtk_widget_set_vexpand(view->stage, FALSE);
    gtk_widget_set_vexpand(view->sizer, FALSE);
    gtk_widget_set_visible(view->placeholder, FALSE);
    gtk_widget_set_visible(view->popout_button, TRUE);
    apply_page_height(view);
    reconsider(view);
    return FALSE;
}

static void
on_pop_out(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    OfficeView *view = self->office_view;
    GtkWidget *header;

    (void)button;
    if (view == NULL || view->window == NULL)
        return;
    if (view->popout != NULL) {
        gtk_window_present(GTK_WINDOW(view->popout));
        return;
    }

    /*
     * Not transient for the main window and not added to the
     * application: it is meant to go to another monitor and be left
     * there, and the app still ends when its main window closes (dispose
     * closes this one first).
     */
    view->popout = adw_window_new();
    gtk_window_set_title(GTK_WINDOW(view->popout), "Office");
    gtk_window_set_default_size(GTK_WINDOW(view->popout), POPOUT_WIDTH, POPOUT_HEIGHT);
    view->toolbar_view = adw_toolbar_view_new();
    header = adw_header_bar_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view->toolbar_view), header);
    adw_window_set_content(ADW_WINDOW(view->popout), view->toolbar_view);

    /* the whole window, not a share of a page */
    gtk_widget_set_size_request(view->sizer, -1, CLAWT_OFFICE_VIEW_MIN_HEIGHT / 2);
    gtk_widget_set_vexpand(view->stage, TRUE);
    gtk_widget_set_vexpand(view->sizer, TRUE);
    move_drawing(view, NULL);
    gtk_widget_set_visible(view->placeholder, TRUE);
    gtk_widget_set_visible(view->popout_button, FALSE);
    g_signal_connect(view->popout, "close-request", G_CALLBACK(on_popout_close), view);
    gtk_window_present(GTK_WINDOW(view->popout));
    reconsider(view);
}

static void
on_bring_back(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    OfficeView *view = self->office_view;

    (void)button;
    if (view != NULL && view->popout != NULL)
        gtk_window_close(GTK_WINDOW(view->popout));
}

static GtkWidget *
build_toolbar(OfficeView *view)
{
    GtkWidget *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *modes = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *zooms = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *follow_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *follow_label = gtk_label_new("Follow");
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *hint;
    GtkExpression *expression;
    guint i;

    /* 2D / 3D, one segmented control */
    gtk_widget_add_css_class(modes, "linked");
    view->mode_2d = toggle_in(modes, NULL, "2D", "Draw the map from above");
    view->mode_3d = toggle_in(modes, view->mode_2d, "3D",
                              "Draw the 3D level, from an orbiting camera");
    g_signal_connect(view->mode_2d, "toggled", G_CALLBACK(on_mode_toggled), view->window);
    g_signal_connect(view->mode_3d, "toggled", G_CALLBACK(on_mode_toggled), view->window);
    gtk_box_append(GTK_BOX(bar), modes);

    /* beside the switch, and in its mode: clawtilla-office as a program of its own */
    {
        GtkWidget *open_button = gtk_button_new_with_label("Open office");

        gtk_widget_set_tooltip_text(open_button,
                                    "Start clawtilla-office in a window of its own, on "
                                    "this connection, in the mode chosen here");
        g_signal_connect(open_button, "clicked", G_CALLBACK(on_open_office_clicked),
                         view->window);
        gtk_box_append(GTK_BOX(bar), open_button);
    }

    /* Fit, 1x, 2x */
    gtk_widget_add_css_class(zooms, "linked");
    view->zoom_buttons[CLAWT_OFFICE_ZOOM_FIT] = toggle_in(zooms, NULL, "Fit",
        "The whole office, at the largest whole-number scale that fits");
    view->zoom_buttons[CLAWT_OFFICE_ZOOM_1X] = toggle_in(zooms,
        view->zoom_buttons[CLAWT_OFFICE_ZOOM_FIT], "1x",
        "One map pixel per screen point; drag to move around");
    view->zoom_buttons[CLAWT_OFFICE_ZOOM_2X] = toggle_in(zooms,
        view->zoom_buttons[CLAWT_OFFICE_ZOOM_FIT], "2x",
        "Two screen points per map pixel; drag to move around");
    for (i = 0; i < G_N_ELEMENTS(view->zoom_buttons); i++)
        g_signal_connect(view->zoom_buttons[i], "toggled", G_CALLBACK(on_zoom_toggled),
                         view->window);
    gtk_box_append(GTK_BOX(bar), zooms);

    /* whom the camera follows: nobody, then the roster, filled from each snapshot */
    view->follow_names = gtk_string_list_new(NULL);
    gtk_string_list_append(view->follow_names, "Nobody");
    view->follow_ids = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(view->follow_ids, g_strdup(""));
    expression = gtk_property_expression_new(GTK_TYPE_STRING_OBJECT, NULL, "string");
    view->follow = gtk_drop_down_new(G_LIST_MODEL(view->follow_names), expression);
    gtk_drop_down_set_enable_search(GTK_DROP_DOWN(view->follow), TRUE);
    gtk_widget_set_tooltip_text(view->follow,
                                "Keep the camera on one participant as they move about");
    g_signal_connect(view->follow, "notify::selected", G_CALLBACK(on_follow_selected),
                     view->window);
    gtk_widget_add_css_class(follow_label, "dim-label");
    gtk_box_append(GTK_BOX(follow_box), follow_label);
    gtk_box_append(GTK_BOX(follow_box), view->follow);
    gtk_box_append(GTK_BOX(bar), follow_box);

    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(bar), spacer);

    view->popout_button = gtk_button_new_with_label("Pop out");
    gtk_widget_set_tooltip_text(view->popout_button,
                                "Open the office in a window of its own");
    g_signal_connect(view->popout_button, "clicked", G_CALLBACK(on_pop_out), view->window);
    gtk_box_append(GTK_BOX(bar), view->popout_button);
    gtk_box_append(GTK_BOX(column), bar);

    /* the same words as clawtilla-lrg's Office page */
    hint = gtk_label_new("Click somebody to open their conversation; drag or scroll to "
                         "turn the 3D view.");
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(hint), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(hint), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(column), hint);
    return column;
}

/* What the slot shows while the drawing is in its own window. */
static GtkWidget *
build_placeholder(OfficeView *view)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *label = gtk_label_new("The office is open in its own window.");
    GtkWidget *back = gtk_button_new_with_label("Bring back");

    gtk_widget_add_css_class(label, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_hexpand(label, TRUE);
    g_signal_connect(back, "clicked", G_CALLBACK(on_bring_back), view->window);
    gtk_box_append(GTK_BOX(box), label);
    gtk_box_append(GTK_BOX(box), back);
    gtk_widget_set_visible(box, FALSE);
    return box;
}

/* ── The slot ─────────────────────────────────────────────────────── */

void
clawt_gtk_office_view_attach(ClawtWindow *self)
{
    OfficeView *view;
    GtkWidget *box;
    GtkEventController *motion;
    GtkGesture *click;
    GtkEventController *scroll;

    if (self->office_view != NULL) {
        reconsider(self->office_view);
        return;
    }

    view = g_rc_box_new0(OfficeView);
    view->window = self;
    view->context = g_main_context_ref_thread_default();
    view->has_3d = TRUE;
    view->paintable = g_object_new(CLAWT_TYPE_NEAREST_PAINTABLE, NULL);
    self->office_view = view;

    box = self->office_view_slot;
    gtk_box_set_spacing(GTK_BOX(box), 6);

    view->banner = adw_banner_new("");
    adw_banner_set_button_label(ADW_BANNER(view->banner), "Restart");
    g_signal_connect(view->banner, "button-clicked", G_CALLBACK(on_restart), self);
    gtk_box_append(GTK_BOX(box), view->banner);

    view->note = gtk_label_new("");
    gtk_widget_add_css_class(view->note, "dim-label");
    gtk_label_set_wrap(GTK_LABEL(view->note), TRUE);
    gtk_label_set_xalign(GTK_LABEL(view->note), 0.0f);
    gtk_label_set_selectable(GTK_LABEL(view->note), TRUE);
    gtk_widget_set_visible(view->note, FALSE);
    gtk_box_append(GTK_BOX(box), view->note);

    /*
     * The size comes from the sizer alone -- an overlay measures only its
     * main child -- because a GtkPicture asks for its paintable's size,
     * and the paintable is a frame drawn at the size the picture asked
     * for: a feedback loop with nothing in it to stop at.
     */
    view->placeholder = build_placeholder(view);
    gtk_box_append(GTK_BOX(box), view->placeholder);

    view->drawing = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(view->drawing), build_toolbar(view));
    sync_toolbar(view);

    view->stage = gtk_overlay_new();
    view->sizer = gtk_drawing_area_new();
    /* the page's share once the page is known (watch_page()); a sensible start until then */
    gtk_widget_set_size_request(view->sizer, -1, CLAWT_OFFICE_VIEW_FALLBACK_HEIGHT);
    gtk_widget_set_hexpand(view->sizer, TRUE);
    g_signal_connect(view->sizer, "resize", G_CALLBACK(on_sizer_resize), self);
    g_signal_connect(view->sizer, "notify::scale-factor", G_CALLBACK(on_scale_factor), self);
    gtk_overlay_set_child(GTK_OVERLAY(view->stage), view->sizer);

    view->picture = gtk_picture_new_for_paintable(GDK_PAINTABLE(view->paintable));
    gtk_picture_set_can_shrink(GTK_PICTURE(view->picture), TRUE);
    gtk_picture_set_content_fit(GTK_PICTURE(view->picture), GTK_CONTENT_FIT_CONTAIN);
    gtk_widget_set_halign(view->picture, GTK_ALIGN_FILL);
    gtk_widget_set_valign(view->picture, GTK_ALIGN_FILL);
    gtk_widget_add_css_class(view->picture, "card");
    gtk_overlay_add_overlay(GTK_OVERLAY(view->stage), view->picture);
    gtk_box_append(GTK_BOX(view->drawing), view->stage);
    gtk_box_append(GTK_BOX(box), view->drawing);

    motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), self);
    gtk_widget_add_controller(view->picture, motion);

    click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
    g_signal_connect(click, "pressed", G_CALLBACK(on_pressed), self);
    g_signal_connect(click, "released", G_CALLBACK(on_released), self);
    gtk_widget_add_controller(view->picture, GTK_EVENT_CONTROLLER(click));

    scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect(scroll, "scroll", G_CALLBACK(on_scroll), self);
    gtk_widget_add_controller(view->picture, scroll);

    /* hidden until a snapshot says the office is on */
    gtk_widget_set_visible(box, FALSE);
    g_signal_connect(box, "map", G_CALLBACK(on_slot_map), self);
    g_signal_connect(box, "unmap", G_CALLBACK(on_slot_unmap), self);
}

void
clawt_gtk_office_view_set_enabled(ClawtWindow *self, gboolean enabled)
{
    OfficeView *view = self->office_view;

    if (view == NULL)
        return;
    view->enabled = enabled;
    reconsider(view);
}

void
clawt_gtk_office_view_set_participants(ClawtWindow *self, JsonArray *participants)
{
    OfficeView *view = self->office_view;
    g_autoptr(GPtrArray) ids = NULL;
    g_autoptr(GPtrArray) names = NULL;
    gboolean same;
    guint selected = 0;
    guint i;

    if (view == NULL || view->follow == NULL)
        return;

    /* nobody first, then the roster in the daemon's order */
    ids = g_ptr_array_new_with_free_func(g_free);
    names = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(ids, g_strdup(""));
    g_ptr_array_add(names, g_strdup("Nobody"));
    for (i = 0; participants != NULL && i < json_array_get_length(participants); i++) {
        JsonObject *p = json_array_get_object_element(participants, i);
        const gchar *id = p != NULL ? clawt_json_string(p, "id", NULL) : NULL;

        if (id == NULL || *id == '\0')
            continue;
        g_ptr_array_add(ids, g_strdup(id));
        g_ptr_array_add(names, g_strdup(clawt_json_string(p, "name", id)));
    }

    /* the same people: leave the list alone, so an open popover is not rebuilt under the pointer */
    same = ids->len == view->follow_ids->len;
    for (i = 0; same && i < ids->len; i++)
        same = g_strcmp0(g_ptr_array_index(ids, i), g_ptr_array_index(view->follow_ids, i)) == 0;
    if (same)
        return;

    for (i = 0; i < ids->len; i++)
        if (view->focus != NULL && g_strcmp0(g_ptr_array_index(ids, i), view->focus) == 0)
            selected = i;

    view->syncing = TRUE;
    g_ptr_array_add(names, NULL);
    gtk_string_list_splice(view->follow_names, 0,
                           g_list_model_get_n_items(G_LIST_MODEL(view->follow_names)),
                           (const gchar * const *)names->pdata);
    g_clear_pointer(&view->follow_ids, g_ptr_array_unref);
    view->follow_ids = g_steal_pointer(&ids);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(view->follow), selected);
    view->syncing = FALSE;

    /* whoever was followed has left the office: follow nobody, and say so to the child */
    if (selected == 0 && view->focus != NULL) {
        g_clear_pointer(&view->focus, g_free);
        send_control(view);
    }
}

void
clawt_gtk_office_view_detach(ClawtWindow *self)
{
    OfficeView *view = self->office_view;

    if (view == NULL)
        return;
    stop_child(view, NULL);
}

void
clawt_gtk_office_view_dispose(ClawtWindow *self)
{
    OfficeView *view = self->office_view;

    if (view == NULL)
        return;
    /* the popped-out window first: its widgets' handlers name this window */
    if (view->popout != NULL)
        gtk_window_destroy(GTK_WINDOW(view->popout));
    view->popout = NULL;
    unwatch_page(view);
    stop_child(view, NULL);
    /* every callback still holding the view now sees no window */
    view->window = NULL;
    self->office_view = NULL;
    view_unref(view);
}
