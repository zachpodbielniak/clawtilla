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
 * pages it is not showing).  Leaving the page stops it; coming back
 * starts a fresh one.  A child that dies, or a stream that breaks, is a
 * banner with Restart in the slot, never anything the window has to
 * survive.
 */

#include "clawt-window-private.h"

#include <glib-unix.h>

#include <errno.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <unistd.h>

/* the drawing's height; its width is the page's */
#define VIEW_HEIGHT (420)
/* a size to start at before the slot has been laid out: 16:9 */
#define FALLBACK_WIDTH (746)
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

typedef struct _OfficeView OfficeView;

struct _OfficeView {
    ClawtWindow       *window;      /* not a reference; NULL once the window goes */
    GMainContext      *context;     /* the window's: every source goes here */

    GtkWidget         *banner;      /* AdwBanner: "stopped drawing", Restart */
    GtkWidget         *note;        /* missing binary, unfollowable connection */
    GtkWidget         *stage;       /* GtkOverlay: the size, and the picture on it */
    GtkWidget         *sizer;       /* the overlay's main child, whose size is the view's */
    GtkWidget         *picture;

    gboolean           enabled;     /* the office is on, per the last snapshot */
    gboolean           mapped;      /* the Office page is on screen */

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
};

/* What an async read or wait holds: the view, and which child it was for. */
typedef struct {
    OfficeView *view;               /* a reference */
    guint       generation;
} Ticket;

static void stop_child(OfficeView *view, const gchar *reason);
static void start_child(OfficeView *view);
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
    gtk_picture_set_paintable(GTK_PICTURE(view->picture), GDK_PAINTABLE(texture));
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
 * The sizer is the overlay's main child, so its allocation is exactly
 * the area the picture covers.  A window being dragged wider resizes it
 * every frame; the child gets the size it settles on.
 */
static void
on_sizer_resize(GtkDrawingArea *area, gint width, gint height, gpointer user_data)
{
    ClawtWindow *self = user_data;
    OfficeView *view = self->office_view;
    gint scale = gtk_widget_get_scale_factor(GTK_WIDGET(area));

    if (view == NULL)
        return;
    view->want_width = width * scale;
    view->want_height = height * scale;
    if (view->frames == NULL)
        return;
    drop_source(&view->resize_source);
    view->resize_source = attach_timeout(view, RESIZE_DEBOUNCE_MS, send_resize, view,
                                         NULL);
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
    gint scale;
    gint width;
    gint height;

    if (self == NULL || view->child != NULL || !view->enabled || !view->mapped)
        return;

    show_banner(view, NULL);
    binary = clawt_office_renderer_resolve(&error);
    if (binary == NULL) {
        show_note(view, error->message);
        gtk_widget_set_visible(view->stage, FALSE);
        return;
    }

    scale = gtk_widget_get_scale_factor(view->sizer);
    width = gtk_widget_get_width(view->sizer) > 0 ? gtk_widget_get_width(view->sizer)
                                                  : FALLBACK_WIDTH;
    height = gtk_widget_get_height(view->sizer) > 0 ? gtk_widget_get_height(view->sizer)
                                                    : VIEW_HEIGHT;
    view->want_width = width * scale;
    view->want_height = height * scale;
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

    launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
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
    if (view->child != NULL) {
        GSubprocess *child = g_steal_pointer(&view->child);

        reap(view, child);
        g_object_unref(child);
    }
    if (view->window != NULL) {
        gtk_picture_set_paintable(GTK_PICTURE(view->picture), NULL);
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
    if (!view->enabled || !view->mapped) {
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
    view->stage = gtk_overlay_new();
    view->sizer = gtk_drawing_area_new();
    gtk_widget_set_size_request(view->sizer, -1, VIEW_HEIGHT);
    gtk_widget_set_hexpand(view->sizer, TRUE);
    g_signal_connect(view->sizer, "resize", G_CALLBACK(on_sizer_resize), self);
    gtk_overlay_set_child(GTK_OVERLAY(view->stage), view->sizer);

    view->picture = gtk_picture_new();
    gtk_picture_set_can_shrink(GTK_PICTURE(view->picture), TRUE);
    gtk_picture_set_content_fit(GTK_PICTURE(view->picture), GTK_CONTENT_FIT_CONTAIN);
    gtk_widget_set_halign(view->picture, GTK_ALIGN_FILL);
    gtk_widget_set_valign(view->picture, GTK_ALIGN_FILL);
    gtk_widget_add_css_class(view->picture, "card");
    gtk_overlay_add_overlay(GTK_OVERLAY(view->stage), view->picture);
    gtk_box_append(GTK_BOX(box), view->stage);

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
    stop_child(view, NULL);
    /* every callback still holding the view now sees no window */
    view->window = NULL;
    self->office_view = NULL;
    view_unref(view);
}
