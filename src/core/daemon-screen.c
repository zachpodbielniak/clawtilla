/*
 * daemon-screen.c - The client surface: watching a screen, and taking it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Its own family rather than more branches in daemon-computer.c: the
 * computer verbs are about a machine's lifecycle and these are about
 * what is on its screen right now, and the two have no state in common
 * beyond the agent id.
 */

#include "clawtilla.h"

#include <string.h>

#include "core/clawt-daemon.h"
#include "core/clawt-daemon-private.h"

/*
 * The agent's computer, or NULL with the reason already reported.
 *
 * The computer is built when the agent starts, so a stopped agent has
 * none -- and "no computer" then reads as a configuration mistake rather
 * than as a stopped agent, which is a different thing to go and check.
 * The same distinction daemon-computer.c draws, drawn the same way.
 */
static ClawtComputer *
screen_computer(ClawtDaemon *self, const gchar *agent_id)
{
    ClawtAgent *agent = (agent_id != NULL)
                        ? clawt_agent_manager_get(self->agents, agent_id)
                        : NULL;

    return (agent != NULL) ? clawt_agent_get_computer(agent) : NULL;
}

static ClawtComputerType
screen_configured_type(ClawtDaemon *self, const gchar *agent_id)
{
    ClawtAgentConfig *config = (agent_id != NULL)
        ? clawt_config_get_agent(self->config, agent_id) : NULL;

    if (config == NULL)
        return CLAWT_COMPUTER_NONE;

    return (ClawtComputerType)clawt_agent_config_get_enum(config,
                                                          "computer.type");
}

/*
 * How long a person may hold the screen, from this agent's own config.
 */
static gint64
screen_lease_seconds(ClawtDaemon *self, const gchar *agent_id)
{
    ClawtAgentConfig *config = (agent_id != NULL)
        ? clawt_config_get_agent(self->config, agent_id) : NULL;

    if (config == NULL)
        return 900;

    return clawt_agent_config_get_int(
        config, "computer.desktop.takeover_lease_seconds");
}

static void
add_screen_status(ClawtDaemon *self, JsonBuilder *builder,
                  const gchar *agent_id)
{
    ClawtComputer *computer = screen_computer(self, agent_id);
    ClawtComputerType type = screen_configured_type(self, agent_id);
    ClawtAgentConfig *config = (agent_id != NULL)
        ? clawt_config_get_agent(self->config, agent_id) : NULL;
    gint64 stamp = clawt_observer_get_frame_stamp(self->observer, agent_id);
    const gchar *last_error =
        clawt_observer_get_last_error(self->observer, agent_id);
    const gchar *holder = clawt_takeover_get_holder(self->takeover, agent_id);
    const gchar *ask = clawt_takeover_get_request(self->takeover, agent_id);

    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(
        builder, clawt_enum_to_nick(CLAWT_TYPE_COMPUTER_TYPE, type));

    /*
     * Two questions, and both answers are needed.
     *
     * `has_screen` is about the *type* and is what decides whether a
     * client draws the tab at all; `observable` is about this agent
     * right now, and is what decides whether the tab has a picture or an
     * explanation in it. A client that only had the first would show an
     * empty panel on a stopped agent, and one that only had the second
     * would silently drop the tab whenever the agent was off.
     */
    json_builder_set_member_name(builder, "has_screen");
    json_builder_add_boolean_value(builder,
                                   clawt_computer_type_has_screen(type));

    json_builder_set_member_name(builder, "observable");
    json_builder_add_boolean_value(
        builder, computer != NULL && CLAWT_IS_OBSERVABLE(computer));

    json_builder_set_member_name(builder, "watchers");
    json_builder_add_int_value(
        builder, clawt_observer_subscribers(self->observer, agent_id));

    json_builder_set_member_name(builder, "fps");
    json_builder_add_int_value(
        builder, clawt_observer_get_fps(self->observer, agent_id));

    json_builder_set_member_name(builder, "stamp");
    json_builder_add_int_value(builder, stamp);

    /*
     * Both sizes, because a click has to be scaled between them and
     * neither client can work out the ratio on its own: the picture is
     * downscaled in the compositor, and how far depends on the screen.
     */
    {
        guint frame_width = 0;
        guint frame_height = 0;
        guint screen_width = 0;
        guint screen_height = 0;

        clawt_observer_get_sizes(self->observer, agent_id, &frame_width,
                                 &frame_height, &screen_width,
                                 &screen_height);

        json_builder_set_member_name(builder, "frame_width");
        json_builder_add_int_value(builder, frame_width);
        json_builder_set_member_name(builder, "frame_height");
        json_builder_add_int_value(builder, frame_height);
        json_builder_set_member_name(builder, "screen_width");
        json_builder_add_int_value(builder, screen_width);
        json_builder_set_member_name(builder, "screen_height");
        json_builder_add_int_value(builder, screen_height);
    }

    /*
     * Whether the picture is old enough to be labelled rather than shown
     * as current, decided here so both clients answer the same way.
     */
    json_builder_set_member_name(builder, "stale");
    json_builder_add_boolean_value(
        builder, clawt_frame_is_stale(stamp, g_get_real_time()));

    json_builder_set_member_name(builder, "held");
    json_builder_add_boolean_value(
        builder, clawt_takeover_is_held(self->takeover, agent_id));

    if (holder != NULL)
        clawt_daemon_add_string_member(builder, "holder", holder);

    json_builder_set_member_name(builder, "expires_at");
    json_builder_add_int_value(
        builder, clawt_takeover_get_expires_at(self->takeover, agent_id));

    json_builder_set_member_name(builder, "lease_seconds");
    json_builder_add_int_value(builder,
                               screen_lease_seconds(self, agent_id));

    if (ask != NULL)
        clawt_daemon_add_string_member(builder, "request", ask);

    json_builder_set_member_name(builder, "can_input");
    json_builder_add_boolean_value(
        builder, computer != NULL && CLAWT_IS_OBSERVABLE(computer) &&
                 clawt_observable_can_input(CLAWT_OBSERVABLE(computer)));

    json_builder_set_member_name(builder, "allow_input");
    json_builder_add_boolean_value(
        builder, config != NULL &&
                 clawt_agent_config_get_boolean(
                     config, "computer.desktop.allow_input"));

    /*
     * Taken from the observer's cache, never asked here.
     *
     * A VM's address is read out of the running domain's XML, and
     * `computer.vm.uri` can name a libvirt on another machine -- so
     * asking on this path would be an SSH round trip on the daemon's
     * main context every time a client redrew the panel. Absent rather
     * than empty when there is nothing to open: an address for a VM
     * that is off sends whoever clicks it to debug their viewer.
     */
    {
        const gchar *viewer = clawt_observer_get_viewer(self->observer,
                                                        agent_id);

        if (viewer != NULL)
            clawt_daemon_add_string_member(builder, "viewer", viewer);
    }

    if (last_error != NULL)
        clawt_daemon_add_string_member(builder, "error", last_error);
}

/* ── Events ──────────────────────────────────────────────────────── */

void
clawt_daemon_on_observer_frame(ClawtObserver *observer,
                               const gchar   *agent_id,
                               const gchar   *path,
                               gpointer       user_data)
{
    ClawtDaemon *self = user_data;
    g_autoptr(ClawtEvent) event = NULL;

    (void)observer;
    (void)path;

    if (self->bus == NULL)
        return;

    /*
     * The event names the agent and when, never the path.
     *
     * A client may be on another machine -- that is what connection
     * profiles are for -- so a filename here would be one that works on
     * this host and shows nothing anywhere else. The bytes come from
     * `computer.frame`, exactly as an attachment's do.
     */
    event = clawt_event_new("computer.frame", agent_id);
    clawt_event_set_detail_int(
        event, "stamp",
        clawt_observer_get_frame_stamp(self->observer, agent_id));

    clawt_event_bus_publish(self->bus, event);
}

void
clawt_daemon_on_observer_failed(ClawtObserver *observer,
                                const gchar   *agent_id,
                                const gchar   *message,
                                gpointer       user_data)
{
    ClawtDaemon *self = user_data;
    g_autoptr(ClawtEvent) event = NULL;

    (void)observer;

    if (self->bus == NULL)
        return;

    /*
     * Reported rather than swallowed. A preview that stops updating with
     * nothing said reads as a machine doing nothing, which is close to
     * the opposite of what it usually means.
     */
    event = clawt_event_new("computer.frame_failed", agent_id);
    clawt_event_set_detail(event, "error", message);

    clawt_event_bus_publish(self->bus, event);
}

void
clawt_daemon_on_takeover_changed(ClawtTakeover *takeover,
                                 const gchar   *agent_id,
                                 gpointer       user_data)
{
    ClawtDaemon *self = user_data;
    g_autoptr(ClawtEvent) event = NULL;
    const gchar *holder;

    (void)takeover;

    if (self->bus == NULL)
        return;

    event = clawt_event_new("computer.takeover", agent_id);
    holder = clawt_takeover_get_holder(self->takeover, agent_id);

    clawt_event_set_detail(event, "held",
                           (holder != NULL) ? "true" : "false");

    if (holder != NULL)
        clawt_event_set_detail(event, "holder", holder);

    if (clawt_takeover_get_request(self->takeover, agent_id) != NULL)
        clawt_event_set_detail(
            event, "request",
            clawt_takeover_get_request(self->takeover, agent_id));

    clawt_event_bus_publish(self->bus, event);
}

/* ── Sending one event, off the main context ─────────────────────── */

typedef struct {
    ClawtDaemon    *daemon;
    ClawtIpcPending *pending;
    gchar          *agent_id;
} InputJob;

/*
 * Frees the job, and the token with it when it was never answered.
 *
 * clawt_ipc_pending_respond() takes the token, so every path has to
 * answer exactly once -- a job dropped without answering leaves a client
 * blocked until it times out.
 */
static void
input_job_free(InputJob *job)
{
    if (job == NULL)
        return;

    g_clear_object(&job->daemon);
    g_free(job->agent_id);
    g_free(job);
}

static void
on_input_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    InputJob *job = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonBuilder) builder = json_builder_new();

    if (!clawt_observer_send_input_finish(CLAWT_OBSERVER(source), result,
                                          &error)) {
        clawt_ipc_pending_respond(
            job->pending,
            clawt_ipc_error_new(clawt_ipc_pending_get_request(job->pending),
                                CLAWT_ERROR_FAILED, error->message));
        input_job_free(job);
        return;
    }

    /*
     * A frame straight after, so somebody who has just clicked sees
     * what happened rather than waiting out the interval. Through the
     * same capture path as everything else, so the minimum gap still
     * applies -- typing quickly must not turn into a grab per keystroke
     * down the agent's own connection.
     */
    clawt_observer_refresh(job->daemon->observer, job->agent_id);

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "sent");
    json_builder_add_boolean_value(builder, TRUE);
    json_builder_end_object(builder);

    clawt_ipc_pending_respond(
        job->pending,
        clawt_ipc_response_new(clawt_ipc_pending_get_request(job->pending),
                               json_builder_get_root(builder)));
    input_job_free(job);
}

/* ── The verbs ───────────────────────────────────────────────────── */

JsonNode *
clawt_daemon_handle_screen(
    ClawtDaemon  *self,
    const gchar  *kind,
    JsonNode     *request,
    JsonObject   *payload,
    gboolean     *handled
)
{
    g_autoptr(JsonBuilder) builder = NULL;
    g_autoptr(GError) error = NULL;
    const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");

    builder = json_builder_new();
    *handled = TRUE;

    if (g_strcmp0(kind, "computer.screen") == 0) {
        if (clawt_config_get_agent(self->config, agent_id) == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such agent");

        json_builder_begin_object(builder);
        add_screen_status(self, builder, agent_id);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "computer.observe") == 0) {
        ClawtComputer *computer = screen_computer(self, agent_id);
        ClawtAgentConfig *config = (agent_id != NULL)
            ? clawt_config_get_agent(self->config, agent_id) : NULL;
        gint64 fps;

        if (config == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such agent");

        if (computer == NULL)
            return clawt_ipc_error_new(
                request, CLAWT_ERROR_AGENT_STATE,
                "that agent is not running, so there is no screen to "
                "watch yet. Start it first.");

        /*
         * The client may ask for a rate, and the config is the default.
         * Clamped in one place either way, so a client cannot ask for
         * sixty and neither can a config file.
         */
        fps = clawt_ipc_payload_int(
            payload, "fps",
            clawt_agent_config_get_int(config,
                                       "computer.desktop.observe_fps"));

        if (!clawt_observer_subscribe(
                self->observer, agent_id, computer,
                clawt_ipc_payload_string(payload, "watcher"), fps, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        json_builder_begin_object(builder);
        add_screen_status(self, builder, agent_id);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "computer.observe_stop") == 0) {
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "watchers");
        json_builder_add_int_value(
            builder,
            clawt_observer_unsubscribe(
                self->observer, agent_id,
                clawt_ipc_payload_string(payload, "watcher")));
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "computer.frame") == 0) {
        const gchar *path =
            clawt_observer_get_frame_path(self->observer, agent_id);
        gint64 stamp = clawt_observer_get_frame_stamp(self->observer,
                                                      agent_id);
        g_autofree gchar *contents = NULL;
        g_autofree gchar *encoded = NULL;
        gsize length = 0;

        /*
         * A refresh asked for here rather than by a verb of its own.
         * The web client polls this while the tab is visible and has no
         * subscription of its own between polls, so the poll *is* the
         * request -- and the observer's minimum gap is what stops a
         * browser holding a tab open from grabbing faster than the
         * timer would.
         */
        if (clawt_ipc_payload_boolean(payload, "refresh", FALSE))
            clawt_observer_refresh(self->observer, agent_id);

        if (path == NULL)
            return clawt_ipc_error_new(
                request, CLAWT_ERROR_NOT_FOUND,
                "there is no frame yet: nothing has been captured since "
                "somebody started watching this screen");

        if (!g_file_get_contents(path, &contents, &length, NULL))
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "the last frame is no longer there");

        encoded = g_base64_encode((const guchar *)contents, length);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "mime");
        json_builder_add_string_value(builder, "image/png");
        json_builder_set_member_name(builder, "bytes");
        json_builder_add_int_value(builder, (gint64)length);
        json_builder_set_member_name(builder, "base64");
        json_builder_add_string_value(builder, encoded);
        json_builder_set_member_name(builder, "stamp");
        json_builder_add_int_value(builder, stamp);
        json_builder_set_member_name(builder, "stale");
        json_builder_add_boolean_value(
            builder, clawt_frame_is_stale(stamp, g_get_real_time()));
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "computer.takeover") == 0) {
        if (clawt_config_get_agent(self->config, agent_id) == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such agent");

        if (!clawt_takeover_take(self->takeover, agent_id,
                                 clawt_ipc_payload_string(payload, "holder"),
                                 screen_lease_seconds(self, agent_id),
                                 &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        json_builder_begin_object(builder);
        add_screen_status(self, builder, agent_id);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "computer.release") == 0) {
        clawt_takeover_release(self->takeover, agent_id);

        json_builder_begin_object(builder);
        add_screen_status(self, builder, agent_id);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "computer.input") == 0) {
        ClawtComputer *computer = screen_computer(self, agent_id);
        g_autoptr(ClawtInputEvent) event = NULL;
        InputJob *job;
        gint kind_value;

        if (computer == NULL || !CLAWT_IS_OBSERVABLE(computer))
            return clawt_ipc_error_new(
                request, CLAWT_ERROR_NOT_SUPPORTED,
                "that agent has no screen to send anything to");

        /*
         * Only while you are holding it.  Not because two people would
         * be dangerous -- it is the operator's own screen either way --
         * but because the takeover is the thing that stopped the agent,
         * and input outside one would race the agent for the pointer
         * with nothing on either side saying so.
         */
        if (!clawt_takeover_is_held(self->takeover, agent_id))
            return clawt_ipc_error_new(
                request, CLAWT_ERROR_INVALID_ARGUMENT,
                "take the screen first: sending input without holding it "
                "would fight the agent for the pointer");

        if (!clawt_enum_from_nick(
                CLAWT_TYPE_INPUT_KIND,
                clawt_ipc_payload_string(payload, "kind"), &kind_value))
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "kind must be key, text, click, move "
                                       "or scroll");

        event = clawt_input_event_new((ClawtInputKind)kind_value);
        clawt_input_event_set_text(event,
                                   clawt_ipc_payload_string(payload, "text"));
        event->x = (gint)clawt_ipc_payload_int(payload, "x", 0);
        event->y = (gint)clawt_ipc_payload_int(payload, "y", 0);
        event->button = (guint)clawt_ipc_payload_int(payload, "button", 1);
        event->dx = (gdouble)clawt_ipc_payload_int(payload, "dx", 0);
        event->dy = (gdouble)clawt_ipc_payload_int(payload, "dy", 0);

        job = g_new0(InputJob, 1);
        job->daemon = g_object_ref(self);
        job->agent_id = g_strdup(agent_id);
        job->pending = clawt_ipc_server_defer(self->ipc_server, request);

        if (job->pending == NULL) {
            input_job_free(job);
            return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                       "this request cannot be answered "
                                       "later");
        }

        /*
         * NULL, not a frame. A key sent into a guest is an SSH round
         * trip; waiting for it here would hold the daemon's context for
         * every keystroke somebody typed.
         */
        clawt_observer_send_input_async(self->observer, agent_id, computer,
                                        event, on_input_finished, job);

        return NULL;
    }

    if (g_strcmp0(kind, "computer.control") == 0) {
        const gchar *tool = clawt_ipc_payload_string(payload, "tool");
        gboolean acting = clawt_desktop_tool_is_acting(tool);
        gboolean held = clawt_takeover_is_held(self->takeover, agent_id);

        /*
         * The agent has just done something to its screen, so a frame at
         * the end of this turn is worth taking. Noted here because this
         * is the one place that already sees every desktop action --
         * a hook of its own would be a second place to forget.
         */
        if (acting && !held) {
            clawt_observer_note_touched(self->observer, agent_id);

            /*
             * And into the recording, if one is running.  Desktop tools
             * never pass through ClawtMcpTools -- they go straight from
             * the agent's own MCP client to a compositor through the
             * relay -- so this gate is the only place clawtilla sees
             * them at all. Without it a trace of a task done on a
             * screen would have every command and no clicks.
             */
            clawt_daemon_teach_note_desktop(self, agent_id, tool);
        }

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "allowed");
        json_builder_add_boolean_value(builder, !(acting && held));

        if (acting && held)
            clawt_daemon_add_string_member(builder, "refusal",
                                           clawt_takeover_refusal_text());

        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    *handled = FALSE;
    return NULL;
}
