/*
 * daemon-teach.c - The client surface: teach.*
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Its own family rather than more branches in daemon-skill.c: a skill is
 * a file in a library and a recording is a live thing with a deadline,
 * a backend and somebody's keyboard in it.
 *
 * Three of these verbs wait -- start and stop talk to a compositor, and
 * synthesize talks to a model -- so all three defer and answer from a
 * worker thread. The rest read state the daemon already holds and
 * return at once.
 */

#include "clawtilla.h"

#include <glib/gstdio.h>
#include <string.h>

#include "core/clawt-daemon.h"
#include "core/clawt-daemon-private.h"

/*
 * How many finished recordings may sit around waiting to be turned into
 * skills.
 *
 * Small on purpose. A trace of a demonstration is a transcript of
 * somebody's keyboard, and a directory of those accumulating in a state
 * directory that `docs/agents.org` suggests keeping in git is not a
 * thing to let happen quietly. The clients say how many there are and
 * the CLI can remove one.
 */
#define MAX_TEACH_RECORDINGS (32)

static gchar *
teach_root(ClawtDaemon *self)
{
    if (self->state_dir == NULL)
        return NULL;

    return g_build_filename(self->state_dir, "teach", NULL);
}

static gchar *
teach_directory(ClawtDaemon *self, const gchar *id)
{
    g_autofree gchar *root = teach_root(self);

    if (root == NULL || id == NULL || !clawt_is_valid_id(id))
        return NULL;

    return g_build_filename(root, id, NULL);
}

/*
 * The recording an agent is being watched by, if any.
 *
 * Walked rather than kept in a second map keyed by agent: a recording
 * is identified by its own id everywhere else, and a second index is a
 * second thing to keep in step. There are at most a handful.
 */
static ClawtTeachRecorder *
recorder_for_agent(ClawtDaemon *self, const gchar *agent_id)
{
    GHashTableIter iter;
    gpointer value;

    if (self->teach_recorders == NULL || agent_id == NULL)
        return NULL;

    g_hash_table_iter_init(&iter, self->teach_recorders);

    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        ClawtTeachRecorder *recorder = value;
        ClawtTeachTrace *trace = clawt_teach_recorder_get_trace(recorder);

        if (!clawt_teach_recorder_is_active(recorder))
            continue;

        if (g_strcmp0(clawt_teach_trace_get_agent_id(trace), agent_id) == 0)
            return recorder;
    }

    return NULL;
}

void
clawt_daemon_teach_note_tool_call(ClawtDaemon *self, const gchar *agent_id,
                                  const gchar *tool, const gchar *args)
{
    ClawtTeachRecorder *recorder;

    g_return_if_fail(CLAWT_IS_DAEMON(self));

    recorder = recorder_for_agent(self, agent_id);

    if (recorder == NULL || !CLAWT_IS_AGENT_TRACE_RECORDER(recorder))
        return;

    clawt_agent_trace_recorder_note_tool_call(
        CLAWT_AGENT_TRACE_RECORDER(recorder), tool, args);
}

void
clawt_daemon_teach_note_desktop(ClawtDaemon *self, const gchar *agent_id,
                                const gchar *tool)
{
    ClawtTeachRecorder *recorder;

    g_return_if_fail(CLAWT_IS_DAEMON(self));

    recorder = recorder_for_agent(self, agent_id);

    if (recorder == NULL || !CLAWT_IS_AGENT_TRACE_RECORDER(recorder))
        return;

    clawt_agent_trace_recorder_note_desktop(
        CLAWT_AGENT_TRACE_RECORDER(recorder), tool);
}

void
clawt_daemon_teach_teardown(ClawtDaemon *self)
{
    GHashTableIter iter;
    gpointer value;

    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (self->teach_recorders != NULL) {
        g_hash_table_iter_init(&iter, self->teach_recorders);

        /*
         * Stopped synchronously, on the way down.
         *
         * The async form would post a worker and a callback onto a
         * context that is about to stop being iterated, so the stop
         * would never complete -- and for a demonstration that means
         * the compositor keeps recording after clawtilla has gone,
         * with the red frame still on the screen and nobody holding
         * the token that would end it.
         */
        while (g_hash_table_iter_next(&iter, NULL, &value)) {
            ClawtTeachRecorder *recorder = value;

            if (clawt_teach_recorder_is_active(recorder))
                clawt_teach_recorder_stop(recorder, "clawtilla stopped",
                                          NULL);
        }

        g_clear_pointer(&self->teach_recorders, g_hash_table_unref);
    }

    g_clear_pointer(&self->teach_drafts, g_hash_table_unref);
}

/* ── Rendering ───────────────────────────────────────────────────── */

static void
add_trace_object(JsonBuilder *builder, ClawtTeachTrace *trace,
                 gboolean active, gboolean with_steps)
{
    g_autoptr(JsonNode) node = clawt_teach_trace_to_json(trace, with_steps);
    JsonObject *object = json_node_get_object(node);
    GList *members = json_object_get_members(object);
    GList *l;

    json_builder_begin_object(builder);

    for (l = members; l != NULL; l = l->next)
        json_builder_set_member_name(builder, l->data),
            json_builder_add_value(
                builder,
                json_node_copy(json_object_get_member(object, l->data)));

    json_builder_set_member_name(builder, "active");
    json_builder_add_boolean_value(builder, active);

    json_builder_end_object(builder);

    g_list_free(members);
}

/* ── Deferred work ───────────────────────────────────────────────── */

typedef struct {
    ClawtDaemon        *daemon;
    ClawtTeachRecorder *recorder;
    ClawtIpcPending    *pending;
} RecorderJob;

static void
recorder_job_free(RecorderJob *job)
{
    if (job == NULL)
        return;

    g_clear_object(&job->daemon);
    g_clear_object(&job->recorder);
    g_free(job);
}

static void
answer_with_trace(RecorderJob *job)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();

    add_trace_object(builder, clawt_teach_recorder_get_trace(job->recorder),
                     clawt_teach_recorder_is_active(job->recorder), FALSE);

    clawt_ipc_pending_respond(
        job->pending,
        clawt_ipc_response_new(clawt_ipc_pending_get_request(job->pending),
                               json_builder_get_root(builder)));
}

static void
on_started(GObject *source, GAsyncResult *result, gpointer user_data)
{
    RecorderJob *job = user_data;
    g_autoptr(GError) error = NULL;

    if (!clawt_teach_recorder_start_finish(CLAWT_TEACH_RECORDER(source),
                                           result, &error)) {
        /*
         * The failed recorder is dropped rather than left in the table.
         * A recording that never started would otherwise occupy its id,
         * so the obvious next thing -- turning consent on and trying
         * again -- would be refused as a duplicate.
         */
        g_hash_table_remove(job->daemon->teach_recorders,
                            clawt_teach_recorder_get_id(job->recorder));

        clawt_ipc_pending_respond(
            job->pending,
            clawt_ipc_error_new(clawt_ipc_pending_get_request(job->pending),
                                error->code, error->message));
        recorder_job_free(job);
        return;
    }

    clawt_event_bus_emit(job->daemon->bus, "teach.changed",
                         clawt_teach_recorder_get_id(job->recorder));
    answer_with_trace(job);
    recorder_job_free(job);
}

static void
on_stopped(GObject *source, GAsyncResult *result, gpointer user_data)
{
    RecorderJob *job = user_data;
    g_autoptr(GError) error = NULL;

    /*
     * A backend that failed to stop cleanly is reported *with* the
     * trace rather than instead of it. The recording has ended either
     * way -- the base guarantees that -- and answering with only an
     * error would leave somebody thinking their demonstration was lost
     * when it is on disk.
     */
    if (!clawt_teach_recorder_stop_finish(CLAWT_TEACH_RECORDER(source),
                                          result, &error) && error != NULL)
        g_message("teach: %s", error->message);

    clawt_event_bus_emit(job->daemon->bus, "teach.changed",
                         clawt_teach_recorder_get_id(job->recorder));
    answer_with_trace(job);
    recorder_job_free(job);
}

typedef struct {
    ClawtDaemon           *daemon;
    ClawtSkillSynthesizer *synthesizer;
    ClawtTeachTrace       *trace;
    ClawtIpcPending       *pending;
    gchar                 *id;
    gchar                 *preview;
    gchar                 *message;
    gboolean               ok;
} SynthesisJob;

static void
synthesis_job_free(SynthesisJob *job)
{
    if (job == NULL)
        return;

    g_clear_object(&job->daemon);
    g_clear_object(&job->synthesizer);
    g_clear_pointer(&job->trace, clawt_teach_trace_unref);
    g_free(job->id);
    g_free(job->preview);
    g_free(job->message);
    g_free(job);
}

static void
synthesis_worker(GTask *task, gpointer source, gpointer data,
                 GCancellable *cancellable)
{
    SynthesisJob *job = data;
    g_autoptr(GError) error = NULL;

    (void)source;
    (void)cancellable;

    if (clawt_skill_synthesizer_synthesize(job->synthesizer, job->trace,
                                            NULL, &error) != NULL) {
        job->ok = TRUE;
        job->preview = clawt_skill_synthesizer_preview(job->synthesizer);
    } else {
        job->message = g_strdup(error->message);
    }

    g_task_return_boolean(task, TRUE);
}

static void
on_synthesised(GObject *source, GAsyncResult *result, gpointer user_data)
{
    SynthesisJob *job = user_data;
    g_autoptr(JsonBuilder) builder = json_builder_new();
    GHashTable *draft;

    (void)source;
    (void)result;

    if (!job->ok) {
        clawt_ipc_pending_respond(
            job->pending,
            clawt_ipc_error_new(clawt_ipc_pending_get_request(job->pending),
                                CLAWT_ERROR_AI,
                                (job->message != NULL) ? job->message
                                                       : "the model wrote "
                                                         "nothing"));
        synthesis_job_free(job);
        return;
    }

    /*
     * Kept so that teach.commit writes exactly what was previewed.  A
     * commit that ran the model again would create something nobody
     * reviewed, which is the one thing a preview exists to prevent --
     * the same reason design.commit keeps its designer.
     */
    if (g_hash_table_size(job->daemon->teach_drafts) >=
        MAX_TEACH_RECORDINGS) {
        GHashTableIter iter;
        gpointer oldest = NULL;

        g_hash_table_iter_init(&iter, job->daemon->teach_drafts);

        if (g_hash_table_iter_next(&iter, &oldest, NULL))
            g_hash_table_remove(job->daemon->teach_drafts, oldest);
    }

    draft = clawt_skill_synthesizer_get_draft(job->synthesizer);

    g_hash_table_insert(job->daemon->teach_drafts, g_strdup(job->id),
                        g_object_ref(job->synthesizer));

    json_builder_begin_object(builder);
    clawt_daemon_add_string_member(builder, "id", job->id);
    clawt_daemon_add_string_member(builder, "name",
                                   g_hash_table_lookup(draft, "name"));
    clawt_daemon_add_string_member(
        builder, "description", g_hash_table_lookup(draft, "description"));
    clawt_daemon_add_string_member(builder, "preview", job->preview);
    clawt_daemon_add_string_member(
        builder, "transcript",
        clawt_skill_synthesizer_get_transcript(job->synthesizer));

    /*
     * Said in the reply, not only in the docs.  Somebody looking at a
     * draft is deciding whether to enable it, and "it will land
     * disabled" is the sentence that stops them going looking for the
     * switch that did not fire.
     */
    clawt_daemon_add_string_member(
        builder, "note",
        "This draft is not a skill yet. Commit it and it lands disabled, "
        "with the same checks an imported skill gets -- read it before "
        "you enable it.");
    json_builder_end_object(builder);

    clawt_ipc_pending_respond(
        job->pending,
        clawt_ipc_response_new(clawt_ipc_pending_get_request(job->pending),
                               json_builder_get_root(builder)));

    synthesis_job_free(job);
}

/* ── Listing what is on disk ─────────────────────────────────────── */

static void
add_recordings(ClawtDaemon *self, JsonBuilder *builder)
{
    g_autofree gchar *root = teach_root(self);
    g_autoptr(GDir) dir = NULL;
    const gchar *entry;

    json_builder_set_member_name(builder, "recordings");
    json_builder_begin_array(builder);

    dir = (root != NULL) ? g_dir_open(root, 0, NULL) : NULL;

    while (dir != NULL && (entry = g_dir_read_name(dir)) != NULL) {
        g_autofree gchar *directory = g_build_filename(root, entry, NULL);
        g_autoptr(ClawtTeachTrace) trace = NULL;
        ClawtTeachRecorder *recorder;

        if (!g_file_test(directory, G_FILE_TEST_IS_DIR))
            continue;

        trace = clawt_teach_trace_load(directory, NULL);

        if (trace == NULL)
            continue;

        recorder = (self->teach_recorders != NULL)
                   ? g_hash_table_lookup(self->teach_recorders, entry) : NULL;

        /*
         * The live recorder's trace wins over the one on disk.
         *
         * The file is written at start and at stop, so a recording in
         * progress reads as empty from disk -- and a listing that
         * showed it that way would tell somebody their demonstration
         * was not being captured while it was.
         */
        if (recorder != NULL)
            add_trace_object(builder,
                             clawt_teach_recorder_get_trace(recorder),
                             clawt_teach_recorder_is_active(recorder), FALSE);
        else
            add_trace_object(builder, trace, FALSE, FALSE);
    }

    json_builder_end_array(builder);
}

/* ── The verbs ───────────────────────────────────────────────────── */

JsonNode *
clawt_daemon_handle_teach(
    ClawtDaemon  *self,
    const gchar  *kind,
    JsonNode     *request,
    JsonObject   *payload,
    gboolean     *handled
)
{
    g_autoptr(JsonBuilder) builder = NULL;
    g_autoptr(GError) error = NULL;

    builder = json_builder_new();
    *handled = TRUE;

    if (self->teach_recorders == NULL)
        self->teach_recorders = g_hash_table_new_full(
            g_str_hash, g_str_equal, g_free, g_object_unref);

    if (self->teach_drafts == NULL)
        self->teach_drafts = g_hash_table_new_full(
            g_str_hash, g_str_equal, g_free, g_object_unref);

    if (g_strcmp0(kind, "teach.start") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        const gchar *source_nick = clawt_ipc_payload_string(payload,
                                                            "source");
        ClawtAgent *agent;
        ClawtTeachRecorder *recorder = NULL;
        RecorderJob *job;
        g_autofree gchar *id = NULL;
        g_autofree gchar *directory = NULL;
        gint source = CLAWT_TEACH_SOURCE_AGENT;

        if (agent_id == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "an agent is required");

        if (source_nick != NULL &&
            !clawt_enum_from_nick(CLAWT_TYPE_TEACH_SOURCE, source_nick,
                                  &source))
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "that is not a kind of recording");

        agent = clawt_agent_manager_get(self->agents, agent_id);

        if (agent == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such agent");

        if (recorder_for_agent(self, agent_id) != NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_AGENT_STATE,
                                       "that agent is already being "
                                       "recorded");

        if (g_hash_table_size(self->teach_recorders) >=
            MAX_TEACH_RECORDINGS)
            return clawt_ipc_error_new(
                request, CLAWT_ERROR_LOOP_LIMIT,
                "there are too many recordings kept; remove some with "
                "teach.remove first");

        id = clawt_generate_token(NULL);

        if (id == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                       "could not name the recording");

        directory = teach_directory(self, id);

        if (directory == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_CONFIG_INVALID,
                                       "there is no state directory to "
                                       "record into");

        if (source == CLAWT_TEACH_SOURCE_AGENT) {
            ClawtAgentTraceRecorder *trace_recorder =
                clawt_agent_trace_recorder_new(id, directory, agent_id);

            if (trace_recorder != NULL) {
                recorder = CLAWT_TEACH_RECORDER(trace_recorder);

                /*
                 * The frames come from the watch the Screen tab already
                 * uses.  A capture path of its own would be a second
                 * round trip down the agent's own connection for the
                 * same picture.
                 */
                clawt_teach_recorder_set_observer(
                    recorder, self->observer, agent_id,
                    clawt_agent_get_computer(agent),
                    clawt_agent_config_get_int(
                        clawt_config_get_agent(self->config, agent_id),
                        "computer.desktop.observe_fps"));
            }
        } else if (source == CLAWT_TEACH_SOURCE_HOST_DEMO) {
            ClawtDesktop *desktop = clawt_agent_get_desktop(agent);

            /*
             * The grant, checked here as well as at the relay.
             *
             * The relay's tool list stops the *agent*; this stops an
             * operator's own client from starting a keylogger against
             * an agent that was never given the grant -- which is what
             * the setting is about, since the recording it produces
             * lands in that agent's trace.
             */
            if (desktop == NULL ||
                !clawt_desktop_get_allow_recording(desktop))
                return clawt_ipc_error_new(
                    request, CLAWT_ERROR_PERMISSION_DENIED,
                    "recording a demonstration needs "
                    "computer.desktop.allow_recording on this agent, and it "
                    "is off. It is deliberately separate from allow_input: "
                    "capturing what you type is not the same grant as "
                    "moving your pointer.");

            recorder = CLAWT_TEACH_RECORDER(clawt_gowl_demo_recorder_new(
                id, directory, clawt_desktop_get_socket_path(desktop)));
        } else {
            ClawtDesktop *desktop = clawt_agent_get_desktop(agent);
            ClawtGuestDemoRecorder *guest;

            if (desktop == NULL ||
                !clawt_desktop_get_allow_recording(desktop))
                return clawt_ipc_error_new(
                    request, CLAWT_ERROR_PERMISSION_DENIED,
                    "recording a demonstration needs "
                    "computer.desktop.allow_recording on this agent, and it "
                    "is off. It is deliberately separate from allow_input: "
                    "capturing what you type is not the same grant as "
                    "moving your pointer.");

            guest = clawt_guest_demo_recorder_new(
                id, directory, agent_id, clawt_agent_get_computer(agent));

            if (guest == NULL)
                return clawt_ipc_error_new(
                    request, CLAWT_ERROR_NOT_SUPPORTED,
                    "that agent has no VM to demonstrate in; a guest "
                    "demonstration needs computer.type: vm with a desktop "
                    "in it");

            recorder = CLAWT_TEACH_RECORDER(guest);
        }

        if (recorder == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                       "could not build a recorder");

        clawt_teach_trace_set_goal(clawt_teach_recorder_get_trace(recorder),
                                   clawt_ipc_payload_string(payload,
                                                            "goal"));
        clawt_teach_recorder_set_limits(
            recorder,
            clawt_config_get_int(self->config, "skills.teach_max_seconds"),
            clawt_config_get_int(self->config, "skills.teach_max_events"));

        g_hash_table_insert(self->teach_recorders, g_strdup(id), recorder);

        job = g_new0(RecorderJob, 1);
        job->daemon = g_object_ref(self);
        job->recorder = g_object_ref(recorder);
        job->pending = clawt_ipc_server_defer(self->ipc_server, request);

        if (job->pending == NULL) {
            g_hash_table_remove(self->teach_recorders, id);
            recorder_job_free(job);
            return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                       "this request cannot be answered "
                                       "later");
        }

        /*
         * NULL, not a reply.  Starting a demonstration is a round trip
         * to a compositor, and holding the daemon's context for it
         * would stop every agent's messages for as long as the
         * compositor took to answer.
         */
        clawt_teach_recorder_start_async(recorder, on_started, job);

        return NULL;
    }

    if (g_strcmp0(kind, "teach.stop") == 0) {
        const gchar *id = clawt_ipc_payload_string(payload, "id");
        ClawtTeachRecorder *recorder;
        RecorderJob *job;

        recorder = (id != NULL)
                   ? g_hash_table_lookup(self->teach_recorders, id) : NULL;

        if (recorder == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no recording of that name is "
                                       "running");

        job = g_new0(RecorderJob, 1);
        job->daemon = g_object_ref(self);
        job->recorder = g_object_ref(recorder);
        job->pending = clawt_ipc_server_defer(self->ipc_server, request);

        if (job->pending == NULL) {
            recorder_job_free(job);
            return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                       "this request cannot be answered "
                                       "later");
        }

        clawt_teach_recorder_stop_async(recorder, "you stopped it",
                                        on_stopped, job);

        return NULL;
    }

    if (g_strcmp0(kind, "teach.list") == 0) {
        json_builder_begin_object(builder);
        add_recordings(self, builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "teach.show") == 0) {
        const gchar *id = clawt_ipc_payload_string(payload, "id");
        ClawtTeachRecorder *recorder;
        g_autofree gchar *directory = NULL;
        g_autoptr(ClawtTeachTrace) trace = NULL;

        recorder = (id != NULL)
                   ? g_hash_table_lookup(self->teach_recorders, id) : NULL;

        if (recorder != NULL) {
            add_trace_object(builder,
                             clawt_teach_recorder_get_trace(recorder),
                             clawt_teach_recorder_is_active(recorder), TRUE);

            return clawt_ipc_response_new(request,
                                          json_builder_get_root(builder));
        }

        directory = teach_directory(self, id);
        trace = (directory != NULL)
                ? clawt_teach_trace_load(directory, &error) : NULL;

        if (trace == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no recording of that name");

        add_trace_object(builder, trace, FALSE, TRUE);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "teach.remove") == 0) {
        const gchar *id = clawt_ipc_payload_string(payload, "id");
        g_autofree gchar *root = teach_root(self);
        g_autofree gchar *directory = teach_directory(self, id);
        ClawtTeachRecorder *recorder;

        if (directory == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "that is not a recording id");

        recorder = g_hash_table_lookup(self->teach_recorders, id);

        if (recorder != NULL && clawt_teach_recorder_is_active(recorder))
            return clawt_ipc_error_new(request, CLAWT_ERROR_AGENT_STATE,
                                       "that recording is still running; "
                                       "stop it first");

        g_hash_table_remove(self->teach_recorders, id);
        g_hash_table_remove(self->teach_drafts, id);

        /*
         * Bounded by the teach directory, checked per child on the
         * canonical path -- the id came from a client and the root came
         * from a config file, and only one of those has been validated.
         */
        if (!clawt_remove_tree(directory, root, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        clawt_event_bus_emit(self->bus, "teach.changed", id);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "removed");
        json_builder_add_boolean_value(builder, TRUE);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "teach.synthesize") == 0) {
        const gchar *id = clawt_ipc_payload_string(payload, "id");
        g_autofree gchar *directory = teach_directory(self, id);
        g_autoptr(ClawtTeachTrace) trace = NULL;
        g_autoptr(GTask) task = NULL;
        ClawtTeachRecorder *recorder;
        SynthesisJob *job;

        if (self->skills == NULL)
            return clawt_ipc_error_new(
                request, CLAWT_ERROR_NOT_SUPPORTED,
                "skills are turned off for this fleet; set skills.enabled");

        recorder = (id != NULL)
                   ? g_hash_table_lookup(self->teach_recorders, id) : NULL;

        if (recorder != NULL && clawt_teach_recorder_is_active(recorder))
            return clawt_ipc_error_new(request, CLAWT_ERROR_AGENT_STATE,
                                       "that recording is still running; "
                                       "stop it first");

        if (recorder != NULL)
            trace = clawt_teach_trace_ref(
                clawt_teach_recorder_get_trace(recorder));
        else if (directory != NULL)
            trace = clawt_teach_trace_load(directory, &error);

        if (trace == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no recording of that name");

        job = g_new0(SynthesisJob, 1);
        job->daemon = g_object_ref(self);
        job->trace = clawt_teach_trace_ref(trace);
        job->id = g_strdup(id);
        job->synthesizer = clawt_skill_synthesizer_new(self->skills,
                                                       self->config);

        if (clawt_ipc_payload_string(payload, "provider") != NULL) {
            if (!clawt_skill_synthesizer_set_provider_by_name(
                    job->synthesizer,
                    clawt_ipc_payload_string(payload, "provider"),
                    clawt_ipc_payload_string(payload, "model"), &error)) {
                synthesis_job_free(job);
                return clawt_ipc_error_new(request, error->code,
                                           error->message);
            }
        } else if (!clawt_skill_synthesizer_use_configured_provider(
                       job->synthesizer, &error)) {
            synthesis_job_free(job);
            return clawt_ipc_error_new(request, error->code, error->message);
        }

        job->pending = clawt_ipc_server_defer(self->ipc_server, request);

        if (job->pending == NULL) {
            synthesis_job_free(job);
            return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                       "this request cannot be answered "
                                       "later");
        }

        /*
         * On a worker, because this is a model call.  design.agent
         * still does the same work on the daemon's own context, which
         * is why pressing its button stalls the whole fleet; this one
         * does not repeat that.
         */
        task = g_task_new(self, NULL, on_synthesised, job);
        g_task_set_task_data(task, job, NULL);
        g_task_run_in_thread(task, synthesis_worker);

        return NULL;
    }

    if (g_strcmp0(kind, "teach.commit") == 0) {
        const gchar *id = clawt_ipc_payload_string(payload, "id");
        ClawtSkillSynthesizer *synthesizer;
        ClawtSkill *skill;

        synthesizer = (id != NULL)
                      ? g_hash_table_lookup(self->teach_drafts, id) : NULL;

        if (synthesizer == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "there is no draft for that "
                                       "recording; run teach.synthesize "
                                       "first");

        skill = clawt_skill_synthesizer_commit(synthesizer, &error);

        if (skill == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        clawt_event_bus_emit(self->bus, "skill.changed",
                             clawt_skill_get_name(skill));

        json_builder_begin_object(builder);
        clawt_daemon_add_string_member(builder, "name",
                                       clawt_skill_get_name(skill));
        json_builder_set_member_name(builder, "enabled");
        json_builder_add_boolean_value(builder,
                                       clawt_skill_get_enabled(skill));
        clawt_daemon_add_string_member(
            builder, "note",
            "Written, and disabled. Read it before you enable it: it was "
            "written by a model from a recording, and it gets exactly the "
            "checks an imported skill gets.");
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    *handled = FALSE;

    return NULL;
}
