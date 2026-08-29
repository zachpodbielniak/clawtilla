/*
 * clawt-teach-recorder.c - Watching a task being done, so it can be reused
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "teach/clawt-teach-recorder.h"

#include <glib/gstdio.h>

typedef struct {
    ClawtTeachTrace *trace;

    guint            max_seconds;
    guint            max_events;
    guint            poll_interval;

    gboolean         active;
    gboolean         stopping;

    /*
     * The context every source here is attached to, and every async
     * answer arrives on.
     *
     * Taken when the recorder is *built* rather than when a source is
     * armed, which is the one case where the usual rule points the other
     * way.  start() runs on a worker thread for two of the three
     * recorders, and a worker started by g_task_run_in_thread() has no
     * thread-default at all -- so reaching for one there would silently
     * take the global default, which for an embedded daemon is a loop
     * nobody runs.  A deadline on that context is a recording that never
     * ends, which is the exact failure the deadline exists to prevent.
     */
    GMainContext    *context;

    GSource         *deadline;
    GSource         *poll;

    /*
     * Which thread started this.  The observer is not thread-safe and
     * the demonstration recorders produce their steps on a worker, so a
     * frame is only ever attached from here.  Checked rather than left
     * as a convention, because the failure would be a use-after-free in
     * the daemon rather than a wrong picture.
     */
    GThread         *owner;

    ClawtObserver   *observer;
    gchar           *observer_agent;
    gint64           observe_fps;
    ClawtComputer   *computer;
    gchar           *watcher;
    gboolean         subscribed;
    gint64           last_frame_stamp;
    guint            frame_seq;

    /*
     * The trace is written from the daemon's context (an agent's steps)
     * and from a worker thread (a demonstration's drain), never both for
     * one recorder -- but the same base does both, and a lock is
     * cheaper than a rule somebody has to remember which subclass they
     * are in.
     */
    GMutex           lock;
} ClawtTeachRecorderPrivate;

G_DEFINE_TYPE_WITH_PRIVATE(ClawtTeachRecorder, clawt_teach_recorder,
                           G_TYPE_OBJECT)

enum {
    SIGNAL_STOPPED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

#define PRIV(o) \
    ((ClawtTeachRecorderPrivate *) \
     clawt_teach_recorder_get_instance_private(CLAWT_TEACH_RECORDER(o)))

/* ── The refusing defaults ───────────────────────────────────────── */

/*
 * A missing vfunc names the type and refuses.
 *
 * Never TRUE.  A recorder that reported a demonstration started and
 * captured nothing tells the person demonstrating at the end, after they
 * have done the work twice.
 */
static gboolean
refuse(ClawtTeachRecorder *self, const gchar *what, GError **error)
{
    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                "a %s cannot %s a recording",
                G_OBJECT_TYPE_NAME(self), what);

    return FALSE;
}

static gboolean
default_start(ClawtTeachRecorder *self, GError **error)
{
    return refuse(self, "start", error);
}

static gboolean
default_stop(ClawtTeachRecorder *self, GError **error)
{
    return refuse(self, "stop", error);
}

static gboolean
default_drain(ClawtTeachRecorder *self, GError **error)
{
    return refuse(self, "drain", error);
}

/* ── Accessors ───────────────────────────────────────────────────── */

const gchar *
clawt_teach_recorder_get_id(ClawtTeachRecorder *self)
{
    g_return_val_if_fail(CLAWT_IS_TEACH_RECORDER(self), NULL);

    return clawt_teach_trace_get_id(PRIV(self)->trace);
}

ClawtTeachTrace *
clawt_teach_recorder_get_trace(ClawtTeachRecorder *self)
{
    g_return_val_if_fail(CLAWT_IS_TEACH_RECORDER(self), NULL);

    return PRIV(self)->trace;
}

gboolean
clawt_teach_recorder_is_active(ClawtTeachRecorder *self)
{
    g_return_val_if_fail(CLAWT_IS_TEACH_RECORDER(self), FALSE);

    return PRIV(self)->active;
}

void
clawt_teach_recorder_set_limits(ClawtTeachRecorder *self,
                                gint64              max_seconds,
                                gint64              max_events)
{
    ClawtTeachRecorderPrivate *priv;

    g_return_if_fail(CLAWT_IS_TEACH_RECORDER(self));

    priv = PRIV(self);

    /*
     * Zero is the default rather than "no limit", the same decision
     * clawt_observe_clamp_fps() makes and for the same reason: an unset
     * integer key reads as zero, and a recording with no deadline is
     * exactly what the deadline exists to prevent.
     */
    priv->max_seconds = (max_seconds > 0)
                        ? (guint)max_seconds
                        : CLAWT_TEACH_DEFAULT_MAX_SECONDS;
    priv->max_events = (max_events > 0)
                       ? (guint)max_events
                       : CLAWT_TEACH_DEFAULT_MAX_EVENTS;
}

guint
clawt_teach_recorder_get_max_seconds(ClawtTeachRecorder *self)
{
    g_return_val_if_fail(CLAWT_IS_TEACH_RECORDER(self), 0);

    return PRIV(self)->max_seconds;
}

guint
clawt_teach_recorder_get_max_events(ClawtTeachRecorder *self)
{
    g_return_val_if_fail(CLAWT_IS_TEACH_RECORDER(self), 0);

    return PRIV(self)->max_events;
}

void
clawt_teach_recorder_set_poll_interval(ClawtTeachRecorder *self,
                                       guint               seconds)
{
    g_return_if_fail(CLAWT_IS_TEACH_RECORDER(self));

    PRIV(self)->poll_interval = seconds;
}

void
clawt_teach_recorder_set_observer(ClawtTeachRecorder *self,
                                  ClawtObserver      *observer,
                                  const gchar        *agent_id,
                                  ClawtComputer      *computer,
                                  gint64              fps)
{
    ClawtTeachRecorderPrivate *priv;

    g_return_if_fail(CLAWT_IS_TEACH_RECORDER(self));

    priv = PRIV(self);

    g_clear_object(&priv->observer);
    g_clear_object(&priv->computer);
    g_clear_pointer(&priv->observer_agent, g_free);

    if (observer != NULL)
        priv->observer = g_object_ref(observer);

    if (computer != NULL)
        priv->computer = g_object_ref(computer);

    priv->observer_agent = g_strdup(agent_id);
    priv->observe_fps = fps;
}

void
clawt_teach_recorder_add_caveat(ClawtTeachRecorder *self, const gchar *text)
{
    ClawtTeachRecorderPrivate *priv;

    g_return_if_fail(CLAWT_IS_TEACH_RECORDER(self));

    priv = PRIV(self);

    g_mutex_lock(&priv->lock);
    clawt_teach_trace_add_caveat(priv->trace, text);
    g_mutex_unlock(&priv->lock);
}

void
clawt_teach_recorder_note_dropped(ClawtTeachRecorder *self, guint count)
{
    ClawtTeachRecorderPrivate *priv;

    g_return_if_fail(CLAWT_IS_TEACH_RECORDER(self));

    if (count == 0)
        return;

    priv = PRIV(self);

    g_mutex_lock(&priv->lock);
    clawt_teach_trace_add_dropped(priv->trace, count);
    g_mutex_unlock(&priv->lock);
}

void
clawt_teach_recorder_note_suppressed(ClawtTeachRecorder *self, guint count)
{
    ClawtTeachRecorderPrivate *priv;

    g_return_if_fail(CLAWT_IS_TEACH_RECORDER(self));

    if (count == 0)
        return;

    priv = PRIV(self);

    g_mutex_lock(&priv->lock);
    clawt_teach_trace_add_suppressed(priv->trace, count);
    g_mutex_unlock(&priv->lock);
}

/* ── Frames ──────────────────────────────────────────────────────── */

/*
 * Copies the observer's latest frame into the trace, once per frame.
 *
 * The observer writes one file per agent and replaces it, so a step that
 * pointed at that path would point at whatever was on the screen when
 * somebody last looked -- which for a trace read a week later is a
 * picture of something else entirely.  The bytes are copied at the
 * moment the step happens, and only when the stamp has moved: an agent
 * that made four tool calls between two grabs gets one picture, attached
 * to the first of them, rather than four copies of the same file.
 */
static void
attach_frame(ClawtTeachRecorder *self, ClawtTeachStep *step)
{
    ClawtTeachRecorderPrivate *priv = PRIV(self);
    const gchar *source;
    const gchar *directory;
    g_autofree gchar *name = NULL;
    g_autofree gchar *target = NULL;
    g_autofree gchar *contents = NULL;
    gsize length = 0;
    gint64 stamp;

    if (priv->observer == NULL || priv->observer_agent == NULL)
        return;

    /*
     * The owner thread and nowhere else.  #ClawtObserver is the
     * daemon's, and the two demonstration recorders produce their steps
     * on a worker -- so this is a check rather than a convention,
     * because getting it wrong would be a use-after-free in the daemon
     * rather than a wrong picture.
     */
    if (g_thread_self() != priv->owner)
        return;

    /*
     * Subscribed on the first step rather than at start.
     *
     * start() runs on a worker for the recorders that talk to a
     * compositor, and one entry point that is sometimes on the daemon's
     * thread and sometimes not is the shape this tree has been bitten
     * by before.  A step is always noted from the owner, so the watch
     * is joined from there -- and a recording that never produced a
     * step never cost the agent a frame either.
     */
    if (!priv->subscribed && !clawt_teach_recorder_join_watch(
            self, priv->observe_fps))
        return;

    stamp = clawt_observer_get_frame_stamp(priv->observer,
                                           priv->observer_agent);

    if (stamp <= 0 || stamp == priv->last_frame_stamp)
        return;

    source = clawt_observer_get_frame_path(priv->observer,
                                           priv->observer_agent);
    directory = clawt_teach_trace_get_directory(priv->trace);

    if (source == NULL || directory == NULL)
        return;

    if (!g_file_get_contents(source, &contents, &length, NULL))
        return;

    priv->last_frame_stamp = stamp;
    name = g_strdup_printf("frame-%04u.png", priv->frame_seq++);
    target = g_build_filename(directory, name, NULL);

    if (!g_file_set_contents(target, contents, (gssize)length, NULL))
        return;

    clawt_teach_step_set_frame(step, name);
}

/* ── Steps ───────────────────────────────────────────────────────── */

gboolean
clawt_teach_recorder_note_step(ClawtTeachRecorder *self,
                               ClawtTeachStep     *step)
{
    ClawtTeachRecorderPrivate *priv;
    gint64 now;
    gboolean kept;

    g_return_val_if_fail(CLAWT_IS_TEACH_RECORDER(self), FALSE);
    g_return_val_if_fail(step != NULL, FALSE);

    priv = PRIV(self);
    now = g_get_real_time();

    /*
     * Only a step that carries no time of its own is stamped here.  A
     * demonstration's events arrive with the compositor's own wall clock
     * on them, and restamping would make every event look as if it had
     * happened at the moment we happened to drain -- which is exactly
     * the "one clock standing in for another" mistake both upstream
     * recorders were shaped to avoid.
     */
    if (clawt_teach_step_get_wall_us(step) == 0)
        clawt_teach_step_set_times(
            step, now,
            (clawt_teach_trace_get_started_at(priv->trace) > 0)
            ? now - clawt_teach_trace_get_started_at(priv->trace) : 0);

    attach_frame(self, step);

    g_mutex_lock(&priv->lock);

    kept = clawt_teach_trace_get_steps(priv->trace)->len < priv->max_events;

    if (kept)
        clawt_teach_trace_add_step(priv->trace, step);
    else
        clawt_teach_trace_add_dropped(priv->trace, 1);

    g_mutex_unlock(&priv->lock);

    if (!kept)
        clawt_teach_step_free(step);

    return kept;
}

/* ── Timers ──────────────────────────────────────────────────────── */

static void on_stop_done(GObject *source, GAsyncResult *result,
                         gpointer user_data);

static gboolean
on_deadline(gpointer user_data)
{
    ClawtTeachRecorder *self = user_data;
    ClawtTeachRecorderPrivate *priv = PRIV(self);

    g_clear_pointer(&priv->deadline, g_source_unref);

    /*
     * Asynchronously, even though this is already on the right context.
     * Stopping a demonstration is a round trip to a compositor, and this
     * timer fires on the daemon's own loop -- holding it for the length
     * of that call would stop every agent's messages for as long as the
     * compositor took to answer.
     */
    clawt_teach_recorder_stop_async(
        self, "the recording reached its time limit",
        on_stop_done, NULL);

    return G_SOURCE_REMOVE;
}

static void
on_poll_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
    g_autoptr(GError) error = NULL;

    (void)user_data;

    if (!g_task_propagate_boolean(G_TASK(result), &error) && error != NULL)
        g_debug("teach: a scheduled drain failed: %s", error->message);
}

static void drain_async(ClawtTeachRecorder *self,
                        GAsyncReadyCallback callback, gpointer user_data);

static gboolean
on_poll(gpointer user_data)
{
    ClawtTeachRecorder *self = user_data;

    if (!PRIV(self)->active)
        return G_SOURCE_REMOVE;

    drain_async(self, on_poll_done, NULL);

    return G_SOURCE_CONTINUE;
}

/*
 * Attached to the stored context, never to the thread-default.
 *
 * See the comment on ClawtTeachRecorderPrivate::context: this can be
 * reached from a worker thread, where the thread-default is NULL.
 */
static GSource *
attach_timer(ClawtTeachRecorder *self, guint seconds, GSourceFunc callback)
{
    ClawtTeachRecorderPrivate *priv = PRIV(self);
    GSource *source = g_timeout_source_new_seconds(seconds);

    g_source_set_callback(source, callback, self, NULL);
    g_source_attach(source, priv->context);

    return source;
}

static void
clear_timers(ClawtTeachRecorder *self)
{
    ClawtTeachRecorderPrivate *priv = PRIV(self);

    if (priv->deadline != NULL) {
        g_source_destroy(priv->deadline);
        g_clear_pointer(&priv->deadline, g_source_unref);
    }

    if (priv->poll != NULL) {
        g_source_destroy(priv->poll);
        g_clear_pointer(&priv->poll, g_source_unref);
    }
}

/* ── Start, stop, drain ──────────────────────────────────────────── */

gboolean
clawt_teach_recorder_start(ClawtTeachRecorder *self, GError **error)
{
    ClawtTeachRecorderPrivate *priv;
    ClawtTeachRecorderClass *klass;
    const gchar *directory;

    g_return_val_if_fail(CLAWT_IS_TEACH_RECORDER(self), FALSE);

    priv = PRIV(self);
    klass = CLAWT_TEACH_RECORDER_GET_CLASS(self);

    if (priv->active) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_AGENT_STATE,
                            "that recording is already running");
        return FALSE;
    }

    directory = clawt_teach_trace_get_directory(priv->trace);

    if (directory == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "this recorder has nowhere to write its trace");
        return FALSE;
    }

    if (g_mkdir_with_parents(directory, 0700) != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "could not create %s", directory);
        return FALSE;
    }

    clawt_teach_trace_set_started_at(priv->trace, g_get_real_time());

    if (!klass->start(self, error)) {
        clawt_teach_trace_set_started_at(priv->trace, 0);
        return FALSE;
    }

    priv->active = TRUE;

    priv->deadline = attach_timer(self, priv->max_seconds, on_deadline);

    if (priv->poll_interval > 0)
        priv->poll = attach_timer(self, priv->poll_interval, on_poll);

    /*
     * Written before anything has been captured.  A daemon that is
     * killed mid-demonstration should leave a trace that says a
     * recording was running and what its caveats were, rather than an
     * empty directory somebody has to guess about.
     */
    clawt_teach_trace_save(priv->trace, NULL);

    return TRUE;
}

gboolean
clawt_teach_recorder_stop(ClawtTeachRecorder  *self,
                          const gchar         *reason,
                          GError             **error)
{
    ClawtTeachRecorderPrivate *priv;
    ClawtTeachRecorderClass *klass;
    gboolean ok;

    g_return_val_if_fail(CLAWT_IS_TEACH_RECORDER(self), FALSE);

    priv = PRIV(self);
    klass = CLAWT_TEACH_RECORDER_GET_CLASS(self);

    if (!priv->active) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_AGENT_STATE,
                            "that recording is not running");
        return FALSE;
    }

    ok = klass->stop(self, error);

    /*
     * Marked stopped whether or not the backend managed it.
     *
     * A compositor that exited mid-demonstration answers nothing, and a
     * recorder that stayed "running" because of that would be one
     * nobody could end -- with the indicator gone, the events gone, and
     * clawtilla still refusing to start another. The failure is
     * reported; the recording ends either way, and the trace it
     * captured is written.
     */
    priv->active = FALSE;
    clear_timers(self);

    if (priv->subscribed && priv->observer != NULL) {
        clawt_observer_unsubscribe(priv->observer, priv->observer_agent,
                                   priv->watcher);
        priv->subscribed = FALSE;
    }

    clawt_teach_trace_set_ended_at(priv->trace, g_get_real_time());
    clawt_teach_trace_set_stop_reason(priv->trace, reason);
    clawt_teach_trace_save(priv->trace, NULL);

    g_signal_emit(self, signals[SIGNAL_STOPPED], 0, reason);

    return ok;
}

gboolean
clawt_teach_recorder_drain(ClawtTeachRecorder *self, GError **error)
{
    ClawtTeachRecorderPrivate *priv;

    g_return_val_if_fail(CLAWT_IS_TEACH_RECORDER(self), FALSE);

    priv = PRIV(self);

    if (!priv->active) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_AGENT_STATE,
                            "that recording is not running");
        return FALSE;
    }

    return CLAWT_TEACH_RECORDER_GET_CLASS(self)->drain(self, error);
}

/* ── The async forms, which is how the IPC handlers reach these ──── */

typedef struct {
    gchar *reason;
} StopRequest;

static void
stop_request_free(gpointer data)
{
    StopRequest *request = data;

    g_free(request->reason);
    g_free(request);
}

static void
start_worker(GTask *task, gpointer source, gpointer data,
             GCancellable *cancellable)
{
    g_autoptr(GError) error = NULL;

    (void)data;
    (void)cancellable;

    if (clawt_teach_recorder_start(CLAWT_TEACH_RECORDER(source), &error))
        g_task_return_boolean(task, TRUE);
    else
        g_task_return_error(task, g_steal_pointer(&error));
}

static void
stop_worker(GTask *task, gpointer source, gpointer data,
            GCancellable *cancellable)
{
    StopRequest *request = data;
    g_autoptr(GError) error = NULL;

    (void)cancellable;

    if (clawt_teach_recorder_stop(CLAWT_TEACH_RECORDER(source),
                                  request->reason, &error))
        g_task_return_boolean(task, TRUE);
    else
        g_task_return_error(task, g_steal_pointer(&error));
}

static void
drain_worker(GTask *task, gpointer source, gpointer data,
             GCancellable *cancellable)
{
    g_autoptr(GError) error = NULL;

    (void)data;
    (void)cancellable;

    if (clawt_teach_recorder_drain(CLAWT_TEACH_RECORDER(source), &error))
        g_task_return_boolean(task, TRUE);
    else
        g_task_return_error(task, g_steal_pointer(&error));
}

/*
 * Every task is created with the recorder's own context pushed.
 *
 * g_task_new() takes the thread-default, and these are started from an
 * IPC dispatch and from a timer callback -- dispatching a source does
 * not push that source's context, so without this the completion lands
 * on a loop nobody runs.
 */
static GTask *
new_task(ClawtTeachRecorder *self, GAsyncReadyCallback callback,
         gpointer user_data)
{
    ClawtTeachRecorderPrivate *priv = PRIV(self);
    GTask *task;

    g_main_context_push_thread_default(priv->context);
    task = g_task_new(self, NULL, callback, user_data);
    g_main_context_pop_thread_default(priv->context);

    return task;
}

void
clawt_teach_recorder_start_async(ClawtTeachRecorder  *self,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
    g_autoptr(GTask) task = NULL;

    g_return_if_fail(CLAWT_IS_TEACH_RECORDER(self));

    task = new_task(self, callback, user_data);
    g_task_run_in_thread(task, start_worker);
}

gboolean
clawt_teach_recorder_start_finish(ClawtTeachRecorder  *self,
                                  GAsyncResult        *result,
                                  GError             **error)
{
    g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

    return g_task_propagate_boolean(G_TASK(result), error);
}

void
clawt_teach_recorder_stop_async(ClawtTeachRecorder  *self,
                                const gchar         *reason,
                                GAsyncReadyCallback  callback,
                                gpointer             user_data)
{
    ClawtTeachRecorderPrivate *priv;
    g_autoptr(GTask) task = NULL;
    StopRequest *request;

    g_return_if_fail(CLAWT_IS_TEACH_RECORDER(self));

    priv = PRIV(self);

    /*
     * One stop at a time.  The deadline and a person clicking Stop can
     * arrive together, and two workers calling the backend's stop with
     * the same token means the second gets an error about a recording
     * that no longer exists -- reported to whoever clicked.
     */
    if (priv->stopping) {
        task = new_task(self, callback, user_data);
        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_AGENT_STATE,
                                "that recording is already stopping");
        return;
    }

    priv->stopping = TRUE;

    request = g_new0(StopRequest, 1);
    request->reason = g_strdup(reason);

    task = new_task(self, callback, user_data);
    g_task_set_task_data(task, request, stop_request_free);
    g_task_run_in_thread(task, stop_worker);
}

gboolean
clawt_teach_recorder_stop_finish(ClawtTeachRecorder  *self,
                                 GAsyncResult        *result,
                                 GError             **error)
{
    g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

    PRIV(self)->stopping = FALSE;

    return g_task_propagate_boolean(G_TASK(result), error);
}

static void
drain_async(ClawtTeachRecorder *self, GAsyncReadyCallback callback,
            gpointer user_data)
{
    g_autoptr(GTask) task = new_task(self, callback, user_data);

    g_task_run_in_thread(task, drain_worker);
}

static void
on_stop_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
    g_autoptr(GError) error = NULL;

    (void)user_data;

    if (!clawt_teach_recorder_stop_finish(CLAWT_TEACH_RECORDER(source),
                                          result, &error) && error != NULL)
        g_message("teach: the recording ended, and its backend did not "
                  "answer cleanly: %s", error->message);
}

/* ── Subscribing to the screen ───────────────────────────────────── */

/*
 * Joins the agent's watch, so steps get pictures.
 *
 * Called by #ClawtAgentTraceRecorder from its own start, on the
 * daemon's thread.  A failure is recorded on the trace and is not a
 * failure to record: an agent with `computer.type: none` has no screen
 * and its trace is steps and no frames, which is a complete answer.
 */
gboolean
clawt_teach_recorder_join_watch(ClawtTeachRecorder *self, gint64 fps)
{
    ClawtTeachRecorderPrivate *priv;
    g_autoptr(GError) error = NULL;

    g_return_val_if_fail(CLAWT_IS_TEACH_RECORDER(self), FALSE);

    priv = PRIV(self);

    if (priv->observer == NULL || priv->observer_agent == NULL ||
        priv->computer == NULL)
        return FALSE;

    if (!clawt_observer_subscribe(priv->observer, priv->observer_agent,
                                  priv->computer, priv->watcher, fps,
                                  &error)) {
        g_autofree gchar *note = g_strdup_printf(
            "No pictures were taken with these steps: %s", error->message);

        /*
         * Recorded and then dropped.  A screen that refused once will
         * refuse on every step, and asking again per step would be one
         * failed round trip per tool call -- so the observer is
         * released here and the trace says, once, why it has no
         * pictures.
         */
        clawt_teach_recorder_add_caveat(self, note);
        g_clear_object(&priv->observer);

        return FALSE;
    }

    priv->subscribed = TRUE;

    return TRUE;
}

/* ── Construction ────────────────────────────────────────────────── */

/*
 * Only for the subclasses' constructors: the trace is the identity, so
 * it is set once and never replaced.
 */
void
clawt_teach_recorder_adopt_trace(ClawtTeachRecorder *self,
                                 ClawtTeachTrace    *trace)
{
    ClawtTeachRecorderPrivate *priv;

    g_return_if_fail(CLAWT_IS_TEACH_RECORDER(self));
    g_return_if_fail(trace != NULL);

    priv = PRIV(self);

    g_clear_pointer(&priv->trace, clawt_teach_trace_unref);
    priv->trace = clawt_teach_trace_ref(trace);

    g_free(priv->watcher);
    priv->watcher = g_strdup_printf("teach:%s",
                                    clawt_teach_trace_get_id(trace));
}

static void
clawt_teach_recorder_dispose(GObject *object)
{
    ClawtTeachRecorder *self = CLAWT_TEACH_RECORDER(object);
    ClawtTeachRecorderPrivate *priv = PRIV(self);

    clear_timers(self);

    if (priv->subscribed && priv->observer != NULL) {
        clawt_observer_unsubscribe(priv->observer, priv->observer_agent,
                                   priv->watcher);
        priv->subscribed = FALSE;
    }

    g_clear_object(&priv->observer);
    g_clear_object(&priv->computer);

    G_OBJECT_CLASS(clawt_teach_recorder_parent_class)->dispose(object);
}

static void
clawt_teach_recorder_finalize(GObject *object)
{
    ClawtTeachRecorderPrivate *priv = PRIV(object);

    g_clear_pointer(&priv->trace, clawt_teach_trace_unref);
    g_clear_pointer(&priv->context, g_main_context_unref);
    g_free(priv->observer_agent);
    g_free(priv->watcher);
    g_mutex_clear(&priv->lock);

    G_OBJECT_CLASS(clawt_teach_recorder_parent_class)->finalize(object);
}

static void
clawt_teach_recorder_class_init(ClawtTeachRecorderClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->dispose = clawt_teach_recorder_dispose;
    object_class->finalize = clawt_teach_recorder_finalize;

    klass->start = default_start;
    klass->stop = default_stop;
    klass->drain = default_drain;

    /**
     * ClawtTeachRecorder::stopped:
     * @self: the recorder
     * @reason: why it stopped
     *
     * The recording has ended, however it ended.
     *
     * Emitted once, from clawt_teach_recorder_stop(), which every path
     * -- a person, the deadline, a shutdown -- goes through.  A signal
     * per way of ending would be three places to forget one.
     */
    signals[SIGNAL_STOPPED] =
        g_signal_new("stopped", CLAWT_TYPE_TEACH_RECORDER,
                     G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL,
                     G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
clawt_teach_recorder_init(ClawtTeachRecorder *self)
{
    ClawtTeachRecorderPrivate *priv = PRIV(self);

    priv->max_seconds = CLAWT_TEACH_DEFAULT_MAX_SECONDS;
    priv->max_events = CLAWT_TEACH_DEFAULT_MAX_EVENTS;
    priv->context = g_main_context_ref_thread_default();
    priv->owner = g_thread_self();

    g_mutex_init(&priv->lock);
}
