/*
 * clawt-gowl-demo-recorder.c - Somebody showing the fleet how, on gowl
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "teach/clawt-gowl-demo-recorder.h"

#include "mcp/clawt-mcp-socket.h"

#include <string.h>

struct _ClawtGowlDemoRecorder {
    ClawtTeachRecorder parent_instance;

    gchar *socket_path;
    gchar *token;
};

G_DEFINE_FINAL_TYPE(ClawtGowlDemoRecorder, clawt_gowl_demo_recorder,
                    CLAWT_TYPE_TEACH_RECORDER)

/*
 * How long to wait on the compositor.
 *
 * Generous compared with a screenshot, because start and stop take the
 * compositor's own lock and it is single-threaded -- and because this
 * is on a worker thread, so a slow answer costs nobody else anything.
 */
#define GOWL_CALL_TIMEOUT_SECONDS (15)

/*
 * How often to take what gowl has buffered.
 *
 * Its ring is 4096 entries and a demonstration with any dragging in it
 * fills that in under a minute, so a recorder that only drained when
 * somebody asked would lose the beginning of the demonstration and
 * report it as a number. Two seconds is short enough that the loss
 * cannot happen and long enough that it is not a poll.
 */
#define GOWL_POLL_SECONDS (2)

ClawtGowlDemoRecorder *
clawt_gowl_demo_recorder_new(const gchar *id,
                             const gchar *directory,
                             const gchar *socket_path)
{
    ClawtGowlDemoRecorder *self;
    g_autoptr(ClawtTeachTrace) trace = NULL;

    g_return_val_if_fail(id != NULL, NULL);
    g_return_val_if_fail(directory != NULL, NULL);

    trace = clawt_teach_trace_new(id, CLAWT_TEACH_SOURCE_HOST_DEMO);

    if (trace == NULL)
        return NULL;

    clawt_teach_trace_set_directory(trace, directory);

    self = g_object_new(CLAWT_TYPE_GOWL_DEMO_RECORDER, NULL);
    self->socket_path = g_strdup(socket_path);

    clawt_teach_recorder_adopt_trace(CLAWT_TEACH_RECORDER(self), trace);
    clawt_teach_recorder_set_poll_interval(CLAWT_TEACH_RECORDER(self),
                                           GOWL_POLL_SECONDS);

    return self;
}

/* ── Reading gowl's payloads ─────────────────────────────────────── */

static const gchar *
member_string(JsonObject *object, const gchar *name)
{
    JsonNode *node;

    if (object == NULL || !json_object_has_member(object, name))
        return NULL;

    node = json_object_get_member(object, name);

    if (node == NULL || !JSON_NODE_HOLDS_VALUE(node))
        return NULL;

    return json_node_get_string(node);
}

static gint64
member_int(JsonObject *object, const gchar *name)
{
    JsonNode *node;

    if (object == NULL || !json_object_has_member(object, name))
        return 0;

    node = json_object_get_member(object, name);

    if (node == NULL || !JSON_NODE_HOLDS_VALUE(node))
        return 0;

    return json_node_get_int(node);
}

static gdouble
member_double(JsonObject *object, const gchar *name)
{
    JsonNode *node;

    if (object == NULL || !json_object_has_member(object, name))
        return 0.0;

    node = json_object_get_member(object, name);

    if (node == NULL || !JSON_NODE_HOLDS_VALUE(node))
        return 0.0;

    return json_node_get_double(node);
}

/*
 * One recorded event, as a step -- or NULL for one that is not a step.
 *
 * Motion and modifier events return NULL. See the header: a drag is
 * hundreds of motions and they would evict the clicks, and every button
 * and scroll already carries the position it happened at.
 */
static ClawtTeachStep *
step_from_event(JsonObject *event)
{
    const gchar *type = member_string(event, "type");
    ClawtTeachStep *step = NULL;

    if (type == NULL)
        return NULL;

    if (g_strcmp0(type, "key") == 0) {
        const gchar *sym = member_string(event, "keysym");
        const gchar *state = member_string(event, "state");
        g_autofree gchar *label = NULL;

        /*
         * Only the presses.  A release is half of the same keystroke,
         * and a trace with both is twice as long and reads as somebody
         * pressing every key twice.
         */
        if (g_strcmp0(state, "press") != 0)
            return NULL;

        label = g_strdup_printf("Key %s", (sym != NULL) ? sym : "(unnamed)");
        step = clawt_teach_step_new(CLAWT_TEACH_STEP_KEY, label);
    } else if (g_strcmp0(type, "pointer_button") == 0) {
        const gchar *state = member_string(event, "state");
        g_autofree gchar *label = NULL;

        if (g_strcmp0(state, "press") != 0)
            return NULL;

        label = g_strdup_printf("Click button %" G_GINT64_FORMAT
                                " at %.0f,%.0f",
                                member_int(event, "button"),
                                member_double(event, "x"),
                                member_double(event, "y"));
        step = clawt_teach_step_new(CLAWT_TEACH_STEP_POINTER, label);
    } else if (g_strcmp0(type, "pointer_axis") == 0) {
        g_autofree gchar *label = g_strdup_printf(
            "Scroll %s at %.0f,%.0f",
            (member_string(event, "axis") != NULL)
            ? member_string(event, "axis") : "?",
            member_double(event, "x"), member_double(event, "y"));

        step = clawt_teach_step_new(CLAWT_TEACH_STEP_SCROLL, label);
    } else {
        return NULL;
    }

    clawt_teach_step_set_times(step, member_int(event, "wall_us"),
                               (gint64)(member_double(event, "offset_ms") *
                                        1000.0));

    return step;
}

gboolean
clawt_gowl_demo_recorder_absorb(ClawtGowlDemoRecorder  *self,
                                const gchar            *payload,
                                GError                **error)
{
    g_autoptr(JsonParser) parser = NULL;
    ClawtTeachRecorder *base;
    JsonNode *root;
    JsonObject *object;
    JsonArray *events;
    const gchar *caveat;
    const gchar *stop_reason;
    guint i;

    g_return_val_if_fail(CLAWT_IS_GOWL_DEMO_RECORDER(self), FALSE);

    base = CLAWT_TEACH_RECORDER(self);

    if (payload == NULL || *payload == '\0') {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
                            "the compositor answered with nothing");
        return FALSE;
    }

    parser = json_parser_new();

    if (!json_parser_load_from_data(parser, payload, -1, error))
        return FALSE;

    root = json_parser_get_root(parser);
    object = (root != NULL && JSON_NODE_HOLDS_OBJECT(root))
             ? json_node_get_object(root) : NULL;

    if (object == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
                            "the compositor's answer was not a recording "
                            "payload");
        return FALSE;
    }

    /*
     * The caveat first, before any step.
     *
     * gowl repeats it in every payload precisely so that it cannot be
     * lost between layers, and taking it from the payload rather than
     * writing our own copy means clawtilla says whatever gowl says --
     * including if gowl's guard changes.
     */
    caveat = member_string(object, "secret_suppression");

    if (caveat != NULL)
        clawt_teach_recorder_add_caveat(base, caveat);
    else
        clawt_teach_recorder_add_caveat(base, CLAWT_TEACH_HOST_DEMO_CAVEAT);

    clawt_teach_recorder_note_dropped(base,
                                      (guint)member_int(object, "dropped"));
    clawt_teach_recorder_note_suppressed(
        base, (guint)member_int(object, "suppressed"));

    stop_reason = member_string(object, "stop_reason");

    if (stop_reason != NULL)
        clawt_teach_trace_set_stop_reason(
            clawt_teach_recorder_get_trace(base), stop_reason);

    events = (json_object_has_member(object, "events") &&
              JSON_NODE_HOLDS_ARRAY(json_object_get_member(object, "events")))
             ? json_object_get_array_member(object, "events") : NULL;

    for (i = 0; events != NULL && i < json_array_get_length(events); i++) {
        JsonObject *event = json_array_get_object_element(events, i);
        ClawtTeachStep *step;

        if (event == NULL)
            continue;

        step = step_from_event(event);

        if (step != NULL)
            clawt_teach_recorder_note_step(base, step);
    }

    return TRUE;
}

/* ── Talking to gowl ─────────────────────────────────────────────── */

static gchar *
call_tool(ClawtGowlDemoRecorder *self, const gchar *tool,
          JsonNode *arguments, GError **error)
{
    g_autoptr(JsonNode) result = NULL;

    if (self->socket_path == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                            "no compositor socket is configured; set "
                            "computer.desktop.socket");
        return NULL;
    }

    result = clawt_mcp_socket_call(self->socket_path, tool, arguments,
                                   GOWL_CALL_TIMEOUT_SECONDS, error);

    if (result == NULL)
        return NULL;

    return clawt_mcp_socket_result_text(result);
}

static gboolean
gowl_start(ClawtTeachRecorder *recorder, GError **error)
{
    ClawtGowlDemoRecorder *self = CLAWT_GOWL_DEMO_RECORDER(recorder);
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonNode) arguments = NULL;
    g_autofree gchar *text = NULL;
    g_autoptr(JsonParser) parser = NULL;
    JsonObject *object;
    const gchar *token;

    /*
     * The caveat before the call, not after it.
     *
     * A start that is refused because consent is off still leaves a
     * trace directory, and that trace should say what this recorder
     * would have been able to see rather than nothing at all.
     */
    clawt_teach_recorder_add_caveat(recorder, CLAWT_TEACH_HOST_DEMO_CAVEAT);

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "max_seconds");
    json_builder_add_int_value(
        builder, clawt_teach_recorder_get_max_seconds(recorder));
    json_builder_set_member_name(builder, "max_events");
    json_builder_add_int_value(
        builder, clawt_teach_recorder_get_max_events(recorder));
    json_builder_end_object(builder);

    arguments = json_builder_get_root(builder);
    text = call_tool(self, "start_recording", arguments, error);

    if (text == NULL)
        return FALSE;

    parser = json_parser_new();

    if (!json_parser_load_from_data(parser, text, -1, error))
        return FALSE;

    object = JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))
             ? json_node_get_object(json_parser_get_root(parser)) : NULL;
    token = member_string(object, "token");

    if (token == NULL) {
        /*
         * gowl's own words.  "Recording is off" and "the compositor is
         * not answering" have different remedies, and a message of our
         * own would lose which of the two happened -- the mistake the
         * desktop relay's refusal hint was written to avoid.
         */
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED,
                    "the compositor did not start a recording: %s", text);
        return FALSE;
    }

    g_free(self->token);
    self->token = g_strdup(token);

    return TRUE;
}

static gboolean
gowl_take(ClawtGowlDemoRecorder *self, const gchar *tool, GError **error)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonNode) arguments = NULL;
    g_autofree gchar *text = NULL;

    if (self->token == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_AGENT_STATE,
                            "this recording never started");
        return FALSE;
    }

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "token");
    json_builder_add_string_value(builder, self->token);
    json_builder_end_object(builder);

    arguments = json_builder_get_root(builder);
    text = call_tool(self, tool, arguments, error);

    if (text == NULL)
        return FALSE;

    return clawt_gowl_demo_recorder_absorb(self, text, error);
}

static gboolean
gowl_stop(ClawtTeachRecorder *recorder, GError **error)
{
    return gowl_take(CLAWT_GOWL_DEMO_RECORDER(recorder), "stop_recording",
                     error);
}

static gboolean
gowl_drain(ClawtTeachRecorder *recorder, GError **error)
{
    return gowl_take(CLAWT_GOWL_DEMO_RECORDER(recorder), "drain_recording",
                     error);
}

static void
clawt_gowl_demo_recorder_finalize(GObject *object)
{
    ClawtGowlDemoRecorder *self = CLAWT_GOWL_DEMO_RECORDER(object);

    g_clear_pointer(&self->socket_path, g_free);
    g_clear_pointer(&self->token, g_free);

    G_OBJECT_CLASS(clawt_gowl_demo_recorder_parent_class)->finalize(object);
}

static void
clawt_gowl_demo_recorder_class_init(ClawtGowlDemoRecorderClass *klass)
{
    ClawtTeachRecorderClass *recorder_class =
        CLAWT_TEACH_RECORDER_CLASS(klass);

    G_OBJECT_CLASS(klass)->finalize = clawt_gowl_demo_recorder_finalize;

    recorder_class->start = gowl_start;
    recorder_class->stop = gowl_stop;
    recorder_class->drain = gowl_drain;
}

static void
clawt_gowl_demo_recorder_init(ClawtGowlDemoRecorder *self)
{
    (void)self;
}
