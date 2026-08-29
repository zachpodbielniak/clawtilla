/*
 * clawt-observer.c - Grabbing frames, but only while somebody is looking
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "computer/clawt-observer.h"
#include "computer/clawt-observable.h"
#include "computer/clawt-screen.h"

#include <glib/gstdio.h>

typedef struct {
    ClawtObserver  *observer;      /* not a reference: the table owns us */
    gchar          *agent_id;
    ClawtComputer  *computer;      /* a reference */

    /*
     * Who is watching, and until when.
     *
     * A count would have done for a window that can say goodbye, and a
     * browser cannot: a closed tab sends nothing, so a count nobody
     * decremented would leave the screen being grabbed for ever. Each
     * watcher carries an expiry that its next poll pushes forward, so a
     * client that stops asking stops being watched -- and a client that
     * subscribes twice is still one watcher rather than two.
     */
    GHashTable     *watchers;      /* watcher id -> gint64 expiry */
    guint           fps;

    GSource        *timer;

    /*
     * One capture at a time.  A second while one is in flight is
     * dropped rather than queued: a queued screenshot arrives showing a
     * moment that has already passed, and a queue of them is a way to
     * keep an agent's connection busy long after anybody stopped
     * looking.
     */
    gboolean        capturing;
    gint64          last_started;  /* monotonic, microseconds */

    gchar          *frame_path;
    gchar          *frame_hash;
    gint64          frame_stamp;   /* real time, microseconds */
    gchar          *last_error;

    /*
     * The picture's size and the screen's, which are not the same
     * number: the frame is downscaled in the compositor. A client that
     * assumed they were equal would put every click a factor of three
     * out on a HiDPI guest -- and it would look like the pointer being
     * unreliable rather than like arithmetic.
     */
    guint           frame_width;
    guint           frame_height;
    guint           screen_width;
    guint           screen_height;

    /*
     * The address a real viewer can open, asked on the worker thread and
     * remembered here.
     *
     * It has to be cached rather than asked when a client wants it: for
     * a VM it is read out of the running domain's XML, and
     * `computer.vm.uri` can name a libvirt on another machine over
     * qemu+ssh -- so answering it from the status handler would put an
     * SSH round trip on the daemon's main context, once per poll, for
     * every client with the Screen tab open.
     */
    gchar          *viewer;

    /*
     * Whether this turn has gone near the screen.  Cleared when the turn
     * settles, so a turn that only wrote a sentence does not end with a
     * picture of a desktop nothing touched.
     */
    gboolean        touched;
} Watch;

struct _ClawtObserver {
    GObject       parent_instance;

    gchar        *frame_dir;
    GMainContext *context;
    GHashTable   *watches;      /* agent id -> Watch */
};

enum {
    SIGNAL_FRAME,
    SIGNAL_FAILED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(ClawtObserver, clawt_observer, G_TYPE_OBJECT)

static void capture_start(ClawtObserver *self, Watch *watch);

static void
watch_stop_timer(Watch *watch)
{
    if (watch->timer == NULL)
        return;

    g_source_destroy(watch->timer);
    g_clear_pointer(&watch->timer, g_source_unref);
}

static void
watch_free(gpointer data)
{
    Watch *watch = data;

    if (watch == NULL)
        return;

    watch_stop_timer(watch);

    if (watch->computer != NULL &&
        CLAWT_IS_OBSERVABLE(watch->computer))
        clawt_observable_stop(CLAWT_OBSERVABLE(watch->computer));

    g_clear_object(&watch->computer);
    g_clear_pointer(&watch->watchers, g_hash_table_unref);
    g_free(watch->agent_id);
    g_free(watch->frame_path);
    g_free(watch->frame_hash);
    g_free(watch->last_error);
    g_free(watch->viewer);
    g_free(watch);
}

/*
 * Drops watchers whose lease has run out, and says how many are left.
 *
 * Applied on every read rather than by a timer, for the reason the
 * takeover lease is: a timer is a promise about when the loop next runs
 * a source, and under load an agent would go on being grabbed for
 * whoever had closed their laptop.
 */
static guint
watch_prune(Watch *watch)
{
    GHashTableIter iter;
    gpointer key;
    gpointer value;
    gint64 now = g_get_monotonic_time();

    if (watch->watchers == NULL)
        return 0;

    g_hash_table_iter_init(&iter, watch->watchers);

    while (g_hash_table_iter_next(&iter, &key, &value)) {
        if (*(gint64 *)value <= now)
            g_hash_table_iter_remove(&iter);
    }

    return g_hash_table_size(watch->watchers);
}

static Watch *
watch_for(ClawtObserver *self, const gchar *agent_id)
{
    if (agent_id == NULL)
        return NULL;

    return g_hash_table_lookup(self->watches, agent_id);
}

/* ── The capture ─────────────────────────────────────────────────── */

typedef struct {
    ClawtObserver *observer;      /* a reference */
    gchar         *agent_id;
    ClawtComputer *computer;      /* a reference */
    gchar         *if_changed_from;

    /* Filled in on the worker thread. */
    GBytes        *bytes;
    gchar         *hash;
    gint64         stamp;
    guint          screen_width;
    guint          screen_height;
    gchar         *viewer;
} CaptureJob;

static void
capture_job_free(gpointer data)
{
    CaptureJob *job = data;

    if (job == NULL)
        return;

    g_clear_object(&job->observer);
    g_clear_object(&job->computer);
    g_clear_pointer(&job->bytes, g_bytes_unref);
    g_free(job->agent_id);
    g_free(job->if_changed_from);
    g_free(job->hash);
    g_free(job->viewer);
    g_free(job);
}

/*
 * Runs on a worker thread, and has to.
 *
 * Every backend here talks to a compositor over a socket or an SSH
 * connection; a grab against a guest that has stopped answering blocks
 * for the connect timeout. On the daemon's main context that would be
 * every agent's messages, every task delivery and every timer stopped
 * for as long as it took -- the rule this tree has already had to apply
 * to computer.exec and to the lifecycle verbs.
 */
static void
capture_thread(GTask        *task,
               gpointer      source_object,
               gpointer      task_data,
               GCancellable *cancellable)
{
    CaptureJob *job = task_data;
    g_autoptr(GError) error = NULL;

    (void)source_object;
    (void)cancellable;

    job->bytes = clawt_observable_frame(CLAWT_OBSERVABLE(job->computer),
                                        job->if_changed_from, &job->stamp,
                                        &job->hash, &error);

    if (job->bytes == NULL && error != NULL) {
        g_task_return_error(task, g_steal_pointer(&error));
        return;
    }

    /*
     * Asked here, on the same thread and in the same round trip, because
     * it can block -- for gowl it is another socket call. Asking it from
     * the status handler would put a compositor round trip on the
     * daemon's main context every time a client drew the panel.
     */
    clawt_observable_geometry(CLAWT_OBSERVABLE(job->computer),
                              &job->screen_width, &job->screen_height);

    /*
     * And the viewer's address, for the same reason and on the same
     * trip: for a VM it comes out of the running domain's XML, which for
     * a remote libvirt is an SSH round trip. Asked once per grab rather
     * than once per client redraw -- and re-asked each time, because a
     * VM restarted between two grabs comes back on a different port.
     */
    job->viewer = clawt_observable_viewer_uri(CLAWT_OBSERVABLE(job->computer));

    g_task_return_boolean(task, TRUE);
}

static void
capture_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    CaptureJob *job = user_data;
    ClawtObserver *self = job->observer;
    Watch *watch;
    g_autoptr(GError) error = NULL;

    (void)source;

    watch = watch_for(self, job->agent_id);

    /*
     * The watch can be gone: the agent was removed, or the last client
     * closed its tab while the grab was in flight. Dropping the result
     * is right, and the flag it would have cleared went with it.
     */
    if (watch == NULL)
        return;

    watch->capturing = FALSE;

    if (!g_task_propagate_boolean(G_TASK(result), &error)) {
        g_free(watch->last_error);
        watch->last_error = g_strdup((error != NULL) ? error->message
                                                     : "the screen could "
                                                       "not be grabbed");

        g_signal_emit(self, signals[SIGNAL_FAILED], 0, job->agent_id,
                      watch->last_error);
        return;
    }

    g_clear_pointer(&watch->last_error, g_free);

    if (job->screen_width > 0 && job->screen_height > 0) {
        watch->screen_width = job->screen_width;
        watch->screen_height = job->screen_height;
    }

    /*
     * Replaced whether or not there is one. NULL means the domain is not
     * running, and holding on to the last address would offer a link
     * that reaches nothing -- or, once the port is reused, somebody
     * else's guest.
     */
    g_free(watch->viewer);
    watch->viewer = g_strdup(job->viewer);

    /*
     * Nothing came back and nothing went wrong: the screen has not
     * moved. The frame already on disk is still the current one, so its
     * stamp is advanced -- otherwise a still desktop would be reported
     * as an ever more stale picture of itself, which is exactly the
     * wrong thing to tell somebody supervising a machine.
     */
    if (job->bytes == NULL) {
        watch->frame_stamp = (job->stamp > 0) ? job->stamp
                                              : g_get_real_time();
        g_signal_emit(self, signals[SIGNAL_FRAME], 0, job->agent_id,
                      watch->frame_path);
        return;
    }

    {
        g_autofree gchar *name = g_strconcat(job->agent_id, ".png", NULL);
        g_autofree gchar *target =
            g_build_filename(self->frame_dir, name, NULL);
        g_autoptr(GError) write_error = NULL;

        /*
         * g_file_set_contents() writes a temporary and renames, so a
         * client reading the file never sees half a PNG -- which
         * decodes as a corrupt image rather than as an error, and would
         * surface as "the screen looks broken" far away from here.
         */
        if (!g_file_set_contents(target,
                                 g_bytes_get_data(job->bytes, NULL),
                                 (gssize)g_bytes_get_size(job->bytes),
                                 &write_error)) {
            g_free(watch->last_error);
            watch->last_error = g_strdup(write_error->message);
            g_signal_emit(self, signals[SIGNAL_FAILED], 0, job->agent_id,
                          watch->last_error);
            return;
        }

        g_free(watch->frame_path);
        watch->frame_path = g_steal_pointer(&target);
    }

    g_free(watch->frame_hash);
    watch->frame_hash = g_strdup(job->hash);
    watch->frame_stamp = (job->stamp > 0) ? job->stamp : g_get_real_time();

    /*
     * From the image's own header, not from what was asked for. The
     * compositor does not upscale, so a screen narrower than the ceiling
     * comes back at its own width -- and a client scaling by the number
     * we requested would be scaling by a size no file ever had.
     */
    clawt_screen_png_size(job->bytes, &watch->frame_width,
                          &watch->frame_height);

    g_signal_emit(self, signals[SIGNAL_FRAME], 0, job->agent_id,
                  watch->frame_path);
}

static void
capture_start(ClawtObserver *self, Watch *watch)
{
    CaptureJob *job;
    GTask *task;
    gint64 now = g_get_monotonic_time();

    if (watch->capturing)
        return;

    /*
     * The minimum gap, applied here rather than at each caller.
     *
     * The timer and the refresh button both arrive here, and a gap
     * enforced only on the timer would leave the button as a way to
     * grab as fast as somebody can click -- against the connection the
     * agent is using to do its work.
     */
    if (watch->last_started != 0 &&
        now - watch->last_started <
            (gint64)CLAWT_OBSERVE_MIN_GAP_MS * 1000)
        return;

    if (watch->computer == NULL || !CLAWT_IS_OBSERVABLE(watch->computer))
        return;

    watch->capturing = TRUE;
    watch->last_started = now;

    job = g_new0(CaptureJob, 1);
    job->observer = g_object_ref(self);
    job->agent_id = g_strdup(watch->agent_id);
    job->computer = g_object_ref(watch->computer);
    job->if_changed_from = g_strdup(watch->frame_hash);

    /*
     * The context is pushed around g_task_new(), not named at the call
     * site.  g_task_new() captures whatever is thread-default *now*, and
     * this is reached from a timer dispatch -- which pushes nothing --
     * so without this the answer would be delivered on a loop nobody
     * runs and the preview would simply never update.
     */
    if (self->context != NULL)
        g_main_context_push_thread_default(self->context);

    task = g_task_new(self, NULL, capture_finished, job);
    g_task_set_task_data(task, job, capture_job_free);
    g_task_run_in_thread(task, capture_thread);
    g_object_unref(task);

    if (self->context != NULL)
        g_main_context_pop_thread_default(self->context);
}

static gboolean
on_tick(gpointer user_data)
{
    Watch *watch = user_data;
    ClawtObserver *observer = watch->observer;
    g_autofree gchar *agent_id = g_strdup(watch->agent_id);

    /*
     * The last lease running out stops the watch here, which is the
     * only place a browser's silence can be noticed. Removing the watch
     * destroys this source, so nothing after it may touch @watch.
     */
    if (watch_prune(watch) == 0) {
        g_hash_table_remove(observer->watches, agent_id);
        return G_SOURCE_REMOVE;
    }

    capture_start(observer, watch);

    return G_SOURCE_CONTINUE;
}

static void
watch_start_timer(ClawtObserver *self, Watch *watch)
{
    guint interval_ms;

    watch_stop_timer(watch);

    interval_ms = 1000 / (watch->fps > 0 ? watch->fps : 1);

    /*
     * Attached to the context this observer was built with, never
     * g_timeout_add()'s global default. For an embedded daemon the
     * global default is a loop nobody runs, and the symptom is a preview
     * panel that stays on its first frame for ever with nothing logged.
     */
    watch->timer = g_timeout_source_new(interval_ms);
    g_source_set_callback(watch->timer, on_tick, watch, NULL);
    g_source_attach(watch->timer, self->context);
}

/* ── The surface ─────────────────────────────────────────────────── */

ClawtObserver *
clawt_observer_new(const gchar *frame_dir, GMainContext *context)
{
    ClawtObserver *self = g_object_new(CLAWT_TYPE_OBSERVER, NULL);

    self->frame_dir = g_strdup(frame_dir);
    self->context = (context != NULL)
                    ? g_main_context_ref(context)
                    : g_main_context_ref_thread_default();

    if (self->frame_dir != NULL)
        g_mkdir_with_parents(self->frame_dir, 0700);

    return self;
}

/*
 * Records a watcher, or pushes its lease forward.
 */
static void
watch_add_watcher(Watch *watch, const gchar *watcher)
{
    gint64 *expiry = g_new0(gint64, 1);

    *expiry = g_get_monotonic_time() +
              ((gint64)CLAWT_OBSERVE_LEASE_SECONDS * G_USEC_PER_SEC);

    g_hash_table_insert(watch->watchers,
                        g_strdup((watcher != NULL && *watcher != '\0')
                                 ? watcher : "a client"),
                        expiry);
}

gboolean
clawt_observer_subscribe(ClawtObserver  *self,
                         const gchar    *agent_id,
                         ClawtComputer  *computer,
                         const gchar    *watcher,
                         gint64          fps,
                         GError        **error)
{
    Watch *watch;
    guint clamped;

    g_return_val_if_fail(CLAWT_IS_OBSERVER(self), FALSE);
    g_return_val_if_fail(agent_id != NULL, FALSE);
    g_return_val_if_fail(CLAWT_IS_COMPUTER(computer), FALSE);

    /*
     * The frame is written as <agent-id>.png, so the id has to be one.
     * Checked here rather than trusted, for the reason
     * clawt_attachment_path() checks: an id that reached the filesystem
     * unvalidated is a request for a path of somebody else's choosing.
     */
    if (!clawt_is_valid_id(agent_id)) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "that is not an agent id");
        return FALSE;
    }

    if (!CLAWT_IS_OBSERVABLE(computer)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                    "a %s computer has no screen to watch",
                    clawt_enum_to_nick(
                        CLAWT_TYPE_COMPUTER_TYPE,
                        clawt_computer_get_computer_type(computer)));
        return FALSE;
    }

    clamped = clawt_observe_clamp_fps(fps);
    watch = watch_for(self, agent_id);

    if (watch != NULL) {
        watch_add_watcher(watch, watcher);
        return TRUE;
    }

    if (!clawt_observable_start(CLAWT_OBSERVABLE(computer), clamped, error))
        return FALSE;

    watch = g_new0(Watch, 1);
    watch->observer = self;
    watch->agent_id = g_strdup(agent_id);
    watch->computer = g_object_ref(computer);
    watch->watchers = g_hash_table_new_full(g_str_hash, g_str_equal,
                                            g_free, g_free);
    watch->fps = clamped;

    watch_add_watcher(watch, watcher);

    g_hash_table_insert(self->watches, g_strdup(agent_id), watch);

    watch_start_timer(self, watch);

    /*
     * A frame straight away rather than at the first tick. Somebody who
     * has just opened the Screen tab is looking at an empty panel until
     * one arrives, and at one frame a second the first tick is a whole
     * second of nothing with no way to tell it from a failure.
     */
    capture_start(self, watch);

    return TRUE;
}

guint
clawt_observer_unsubscribe(ClawtObserver *self,
                           const gchar   *agent_id,
                           const gchar   *watcher)
{
    Watch *watch;

    g_return_val_if_fail(CLAWT_IS_OBSERVER(self), 0);

    watch = watch_for(self, agent_id);

    if (watch == NULL)
        return 0;

    g_hash_table_remove(watch->watchers,
                        (watcher != NULL && *watcher != '\0')
                        ? watcher : "a client");

    if (watch_prune(watch) > 0)
        return g_hash_table_size(watch->watchers);

    /*
     * The last watcher stops it. Everything the watch remembered goes
     * with it, including the last hash -- so the next subscriber gets a
     * whole frame rather than being told the screen has not changed
     * since a picture it has never seen.
     */
    g_hash_table_remove(self->watches, agent_id);

    return 0;
}

guint
clawt_observer_subscribers(ClawtObserver *self, const gchar *agent_id)
{
    Watch *watch;

    g_return_val_if_fail(CLAWT_IS_OBSERVER(self), 0);

    watch = watch_for(self, agent_id);

    return (watch != NULL) ? watch_prune(watch) : 0;
}

gboolean
clawt_observer_refresh(ClawtObserver *self, const gchar *agent_id)
{
    Watch *watch;

    g_return_val_if_fail(CLAWT_IS_OBSERVER(self), FALSE);

    watch = watch_for(self, agent_id);

    if (watch == NULL)
        return FALSE;

    if (watch->capturing)
        return FALSE;

    capture_start(self, watch);

    return watch->capturing;
}

void
clawt_observer_note_touched(ClawtObserver *self, const gchar *agent_id)
{
    Watch *watch;

    g_return_if_fail(CLAWT_IS_OBSERVER(self));

    watch = watch_for(self, agent_id);

    if (watch != NULL)
        watch->touched = TRUE;
}

void
clawt_observer_settle_turn(ClawtObserver *self, const gchar *agent_id)
{
    Watch *watch;

    g_return_if_fail(CLAWT_IS_OBSERVER(self));

    watch = watch_for(self, agent_id);

    if (watch == NULL || !watch->touched)
        return;

    watch->touched = FALSE;

    capture_start(self, watch);
}

const gchar *
clawt_observer_get_frame_path(ClawtObserver *self, const gchar *agent_id)
{
    Watch *watch;

    g_return_val_if_fail(CLAWT_IS_OBSERVER(self), NULL);

    watch = watch_for(self, agent_id);

    return (watch != NULL) ? watch->frame_path : NULL;
}

gint64
clawt_observer_get_frame_stamp(ClawtObserver *self, const gchar *agent_id)
{
    Watch *watch;

    g_return_val_if_fail(CLAWT_IS_OBSERVER(self), 0);

    watch = watch_for(self, agent_id);

    return (watch != NULL) ? watch->frame_stamp : 0;
}

const gchar *
clawt_observer_get_last_error(ClawtObserver *self, const gchar *agent_id)
{
    Watch *watch;

    g_return_val_if_fail(CLAWT_IS_OBSERVER(self), NULL);

    watch = watch_for(self, agent_id);

    return (watch != NULL) ? watch->last_error : NULL;
}

void
clawt_observer_get_sizes(ClawtObserver *self,
                         const gchar   *agent_id,
                         guint         *frame_width,
                         guint         *frame_height,
                         guint         *screen_width,
                         guint         *screen_height)
{
    Watch *watch;

    g_return_if_fail(CLAWT_IS_OBSERVER(self));

    watch = watch_for(self, agent_id);

    if (frame_width != NULL)
        *frame_width = (watch != NULL) ? watch->frame_width : 0;

    if (frame_height != NULL)
        *frame_height = (watch != NULL) ? watch->frame_height : 0;

    if (screen_width != NULL)
        *screen_width = (watch != NULL) ? watch->screen_width : 0;

    if (screen_height != NULL)
        *screen_height = (watch != NULL) ? watch->screen_height : 0;
}

const gchar *
clawt_observer_get_viewer(ClawtObserver *self, const gchar *agent_id)
{
    Watch *watch;

    g_return_val_if_fail(CLAWT_IS_OBSERVER(self), NULL);

    watch = watch_for(self, agent_id);

    return (watch != NULL) ? watch->viewer : NULL;
}

guint
clawt_observer_get_fps(ClawtObserver *self, const gchar *agent_id)
{
    Watch *watch;

    g_return_val_if_fail(CLAWT_IS_OBSERVER(self), 0);

    watch = watch_for(self, agent_id);

    return (watch != NULL) ? watch->fps : 0;
}

/* ── Sending one event ───────────────────────────────────────────── */

typedef struct {
    ClawtComputer   *computer;   /* a reference */
    ClawtInputEvent *event;
    gchar           *agent_id;
} InputJob;

static void
input_job_free(gpointer data)
{
    InputJob *job = data;

    if (job == NULL)
        return;

    g_clear_object(&job->computer);
    g_clear_pointer(&job->event, clawt_input_event_free);
    g_free(job->agent_id);
    g_free(job);
}

static void
input_thread(GTask        *task,
             gpointer      source_object,
             gpointer      task_data,
             GCancellable *cancellable)
{
    InputJob *job = task_data;
    g_autoptr(GError) error = NULL;

    (void)source_object;
    (void)cancellable;

    if (!clawt_observable_send_input(CLAWT_OBSERVABLE(job->computer),
                                     job->event, &error)) {
        /*
         * What was attempted, in front of why it failed. "the guest's
         * desktop refused" on its own does not say whether a key or a
         * click was refused, and those have different remedies -- and
         * the description deliberately counts a typed string rather
         * than quoting it.
         */
        g_autofree gchar *what = clawt_input_event_describe(job->event);

        g_prefix_error(&error, "%s: ", what);
        g_task_return_error(task, g_steal_pointer(&error));
        return;
    }

    g_task_return_boolean(task, TRUE);
}

void
clawt_observer_send_input_async(ClawtObserver       *self,
                                const gchar         *agent_id,
                                ClawtComputer       *computer,
                                ClawtInputEvent     *event,
                                GAsyncReadyCallback  callback,
                                gpointer             user_data)
{
    InputJob *job;
    GTask *task;

    g_return_if_fail(CLAWT_IS_OBSERVER(self));
    g_return_if_fail(CLAWT_IS_COMPUTER(computer));
    g_return_if_fail(event != NULL);

    job = g_new0(InputJob, 1);
    job->computer = g_object_ref(computer);
    job->event = clawt_input_event_copy(event);
    job->agent_id = g_strdup(agent_id);

    if (self->context != NULL)
        g_main_context_push_thread_default(self->context);

    task = g_task_new(self, NULL, callback, user_data);
    g_task_set_task_data(task, job, input_job_free);
    g_task_run_in_thread(task, input_thread);
    g_object_unref(task);

    if (self->context != NULL)
        g_main_context_pop_thread_default(self->context);
}

gboolean
clawt_observer_send_input_finish(ClawtObserver  *self,
                                 GAsyncResult   *result,
                                 GError        **error)
{
    g_return_val_if_fail(CLAWT_IS_OBSERVER(self), FALSE);
    g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

    return g_task_propagate_boolean(G_TASK(result), error);
}

void
clawt_observer_drop_agent(ClawtObserver *self, const gchar *agent_id)
{
    g_return_if_fail(CLAWT_IS_OBSERVER(self));

    if (agent_id == NULL)
        return;

    g_hash_table_remove(self->watches, agent_id);
}

void
clawt_observer_stop_all(ClawtObserver *self)
{
    g_return_if_fail(CLAWT_IS_OBSERVER(self));

    g_hash_table_remove_all(self->watches);
}

static void
clawt_observer_finalize(GObject *object)
{
    ClawtObserver *self = CLAWT_OBSERVER(object);

    g_clear_pointer(&self->watches, g_hash_table_unref);
    g_clear_pointer(&self->context, g_main_context_unref);
    g_clear_pointer(&self->frame_dir, g_free);

    G_OBJECT_CLASS(clawt_observer_parent_class)->finalize(object);
}

static void
clawt_observer_class_init(ClawtObserverClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = clawt_observer_finalize;

    /**
     * ClawtObserver::frame:
     * @self: the #ClawtObserver
     * @agent_id: whose screen
     * @path: (nullable): where the frame is on this machine
     *
     * A frame is ready -- or the screen has not changed since the last
     * one, which is the same news to whoever is watching.
     */
    signals[SIGNAL_FRAME] =
        g_signal_new("frame", CLAWT_TYPE_OBSERVER, G_SIGNAL_RUN_LAST,
                     0, NULL, NULL, NULL, G_TYPE_NONE, 2,
                     G_TYPE_STRING, G_TYPE_STRING);

    /**
     * ClawtObserver::failed:
     * @self: the #ClawtObserver
     * @agent_id: whose screen
     * @message: the backend's own words
     *
     * A grab failed.  Reported rather than retried silently: a screen
     * that stopped updating with nothing said reads as a machine doing
     * nothing, which is the opposite of what it usually means.
     */
    signals[SIGNAL_FAILED] =
        g_signal_new("failed", CLAWT_TYPE_OBSERVER, G_SIGNAL_RUN_LAST,
                     0, NULL, NULL, NULL, G_TYPE_NONE, 2,
                     G_TYPE_STRING, G_TYPE_STRING);
}

static void
clawt_observer_init(ClawtObserver *self)
{
    self->watches = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                          watch_free);
}
