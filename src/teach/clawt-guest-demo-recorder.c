/*
 * clawt-guest-demo-recorder.c - Somebody showing the fleet how, in a VM
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "teach/clawt-guest-demo-recorder.h"

#include "computer/clawt-screen.h"
#include "computer/clawt-vm-computer.h"

#include <string.h>

struct _ClawtGuestDemoRecorder {
    ClawtTeachRecorder parent_instance;

    ClawtComputer *computer;
    gchar         *token;
};

G_DEFINE_FINAL_TYPE(ClawtGuestDemoRecorder, clawt_guest_demo_recorder,
                    CLAWT_TYPE_TEACH_RECORDER)

/*
 * The extension samples motion every 20 ms and its ring is 2,000 events
 * by default, so a demonstration with any dragging in it loses its
 * beginning within a minute unless somebody is taking the events out.
 */
#define GUEST_POLL_SECONDS (2)

ClawtGuestDemoRecorder *
clawt_guest_demo_recorder_new(const gchar   *id,
                              const gchar   *directory,
                              const gchar   *agent_id,
                              ClawtComputer *computer)
{
    ClawtGuestDemoRecorder *self;
    g_autoptr(ClawtTeachTrace) trace = NULL;

    g_return_val_if_fail(id != NULL, NULL);
    g_return_val_if_fail(directory != NULL, NULL);

    /*
     * Refused here rather than at start.
     *
     * A recorder built against a container would look armed and then
     * fail at the moment somebody was ready to demonstrate, which is
     * the worst time to find out.
     */
    if (computer == NULL || !CLAWT_IS_VM_COMPUTER(computer))
        return NULL;

    trace = clawt_teach_trace_new(id, CLAWT_TEACH_SOURCE_GUEST_DEMO);

    if (trace == NULL)
        return NULL;

    clawt_teach_trace_set_directory(trace, directory);
    clawt_teach_trace_set_agent_id(trace, agent_id);

    self = g_object_new(CLAWT_TYPE_GUEST_DEMO_RECORDER, NULL);
    self->computer = g_object_ref(computer);

    clawt_teach_recorder_adopt_trace(CLAWT_TEACH_RECORDER(self), trace);
    clawt_teach_recorder_set_poll_interval(CLAWT_TEACH_RECORDER(self),
                                           GUEST_POLL_SECONDS);

    return self;
}

/* ── Reading the extension's events ──────────────────────────────── */

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

static ClawtTeachStep *
step_from_event(JsonObject *event, gboolean *suppressed_out)
{
    const gchar *type = member_string(event, "type");
    ClawtTeachStep *step = NULL;

    *suppressed_out = FALSE;

    if (type == NULL)
        return NULL;

    if (g_strcmp0(type, "key_press") == 0) {
        gint64 unicode = member_int(event, "unicode");
        g_autofree gchar *label = NULL;

        /*
         * The codepoint, not the keyval.  The extension reports both,
         * and the codepoint is what a reader can recognise -- a trace
         * full of `keyval 65307` is a trace nobody reads.
         *
         * Printable characters only. A control codepoint written
         * straight into the label would put a bell or a backspace into
         * a JSON file and then into somebody's terminal.
         */
        if (unicode > 0x20 && unicode < 0x110000 &&
            g_unichar_isprint((gunichar)unicode)) {
            gchar buffer[8] = { 0 };
            gint length = g_unichar_to_utf8((gunichar)unicode, buffer);

            buffer[length] = '\0';
            label = g_strdup_printf("Key %s", buffer);
        } else {
            label = g_strdup_printf("Key (keyval %" G_GINT64_FORMAT ")",
                                    member_int(event, "keyval"));
        }

        step = clawt_teach_step_new(CLAWT_TEACH_STEP_KEY, label);
    } else if (g_strcmp0(type, "button_press") == 0) {
        g_autofree gchar *label = g_strdup_printf(
            "Click at %.0f,%.0f", member_double(event, "x"),
            member_double(event, "y"));

        step = clawt_teach_step_new(CLAWT_TEACH_STEP_POINTER, label);
    } else if (g_strcmp0(type, "scroll") == 0) {
        g_autofree gchar *label = g_strdup_printf(
            "Scroll %s at %.0f,%.0f",
            (member_string(event, "direction") != NULL)
            ? member_string(event, "direction") : "?",
            member_double(event, "x"), member_double(event, "y"));

        step = clawt_teach_step_new(CLAWT_TEACH_STEP_SCROLL, label);
    } else if (g_strcmp0(type, "suppressed") == 0) {
        const gchar *why = member_string(event, "reason");
        g_autofree gchar *label = g_strdup_printf(
            "Capture paused: %s", (why != NULL) ? why : "a secret was on "
                                                        "screen");

        *suppressed_out = TRUE;
        step = clawt_teach_step_new(CLAWT_TEACH_STEP_MARKER, label);
    } else if (g_strcmp0(type, "resumed") == 0) {
        step = clawt_teach_step_new(CLAWT_TEACH_STEP_MARKER,
                                    "Capture resumed");
    } else {
        return NULL;
    }

    /*
     * The extension's own wall clock, kept as it arrived.
     *
     * Neither field it emits is in the domain a synthetic timestamp
     * lives in, which is deliberate upstream -- `input.js` had a bug
     * where synthetic times went backwards, and a captured time handed
     * back to an injection call is how that happens again.
     */
    clawt_teach_step_set_times(step, member_int(event, "wall_us"),
                               member_int(event, "offset_us"));

    return step;
}

gboolean
clawt_guest_demo_recorder_absorb(ClawtGuestDemoRecorder  *self,
                                 const gchar             *events_json,
                                 guint                    dropped,
                                 GError                 **error)
{
    g_autoptr(JsonParser) parser = NULL;
    ClawtTeachRecorder *base;
    JsonNode *root;
    JsonArray *events;
    guint suppressed = 0;
    guint i;

    g_return_val_if_fail(CLAWT_IS_GUEST_DEMO_RECORDER(self), FALSE);

    base = CLAWT_TEACH_RECORDER(self);
    clawt_teach_recorder_note_dropped(base, dropped);

    if (events_json == NULL || *events_json == '\0') {
        /*
         * Not an error.  A drain during a quiet moment answers with an
         * empty list, and treating that as a failure would end a
         * demonstration because somebody paused to read something.
         */
        return TRUE;
    }

    parser = json_parser_new();

    if (!json_parser_load_from_data(parser, events_json, -1, error))
        return FALSE;

    root = json_parser_get_root(parser);

    if (root == NULL || !JSON_NODE_HOLDS_ARRAY(root)) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
                            "the guest's desktop did not answer with a "
                            "list of events");
        return FALSE;
    }

    events = json_node_get_array(root);

    for (i = 0; events != NULL && i < json_array_get_length(events); i++) {
        JsonObject *event = json_array_get_object_element(events, i);
        ClawtTeachStep *step;
        gboolean is_suppression = FALSE;

        if (event == NULL)
            continue;

        step = step_from_event(event, &is_suppression);

        if (is_suppression)
            suppressed++;

        if (step != NULL)
            clawt_teach_recorder_note_step(base, step);
    }

    clawt_teach_recorder_note_suppressed(base, suppressed);

    return TRUE;
}

/* ── Talking to the guest ────────────────────────────────────────── */

static gboolean
guest_start(ClawtTeachRecorder *recorder, GError **error)
{
    ClawtGuestDemoRecorder *self = CLAWT_GUEST_DEMO_RECORDER(recorder);
    g_auto(GStrv) argv = NULL;
    g_autofree gchar *reply = NULL;
    g_autofree gchar *token = NULL;

    clawt_teach_recorder_add_caveat(recorder, CLAWT_TEACH_GUEST_DEMO_CAVEAT);

    argv = clawt_screen_gnome_record_start_argv(
        clawt_teach_recorder_get_max_seconds(recorder),
        clawt_teach_recorder_get_max_events(recorder));

    reply = clawt_vm_computer_session_run(CLAWT_VM_COMPUTER(self->computer),
                                          argv, error);

    if (reply == NULL)
        return FALSE;

    token = clawt_screen_parse_gdbus_string(reply);

    if (token == NULL || *token == '\0') {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED,
                    "the guest's desktop did not start a recording: %s",
                    reply);
        return FALSE;
    }

    g_free(self->token);
    self->token = g_steal_pointer(&token);

    return TRUE;
}

static gboolean
guest_take(ClawtGuestDemoRecorder *self, gboolean stopping, GError **error)
{
    g_auto(GStrv) argv = NULL;
    g_autofree gchar *reply = NULL;
    g_autofree gchar *events = NULL;
    guint dropped = 0;

    if (self->token == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_AGENT_STATE,
                            "this recording never started");
        return FALSE;
    }

    argv = stopping ? clawt_screen_gnome_record_stop_argv(self->token)
                    : clawt_screen_gnome_record_drain_argv(self->token);

    reply = clawt_vm_computer_session_run(CLAWT_VM_COMPUTER(self->computer),
                                          argv, error);

    if (reply == NULL)
        return FALSE;

    if (!clawt_screen_parse_gdbus_events(reply, &events, &dropped, error))
        return FALSE;

    return clawt_guest_demo_recorder_absorb(self, events, dropped, error);
}

static gboolean
guest_stop(ClawtTeachRecorder *recorder, GError **error)
{
    return guest_take(CLAWT_GUEST_DEMO_RECORDER(recorder), TRUE, error);
}

static gboolean
guest_drain(ClawtTeachRecorder *recorder, GError **error)
{
    return guest_take(CLAWT_GUEST_DEMO_RECORDER(recorder), FALSE, error);
}

static void
clawt_guest_demo_recorder_dispose(GObject *object)
{
    ClawtGuestDemoRecorder *self = CLAWT_GUEST_DEMO_RECORDER(object);

    g_clear_object(&self->computer);

    G_OBJECT_CLASS(clawt_guest_demo_recorder_parent_class)->dispose(object);
}

static void
clawt_guest_demo_recorder_finalize(GObject *object)
{
    ClawtGuestDemoRecorder *self = CLAWT_GUEST_DEMO_RECORDER(object);

    g_clear_pointer(&self->token, g_free);

    G_OBJECT_CLASS(clawt_guest_demo_recorder_parent_class)->finalize(object);
}

static void
clawt_guest_demo_recorder_class_init(ClawtGuestDemoRecorderClass *klass)
{
    ClawtTeachRecorderClass *recorder_class =
        CLAWT_TEACH_RECORDER_CLASS(klass);

    G_OBJECT_CLASS(klass)->dispose = clawt_guest_demo_recorder_dispose;
    G_OBJECT_CLASS(klass)->finalize = clawt_guest_demo_recorder_finalize;

    recorder_class->start = guest_start;
    recorder_class->stop = guest_stop;
    recorder_class->drain = guest_drain;
}

static void
clawt_guest_demo_recorder_init(ClawtGuestDemoRecorder *self)
{
    (void)self;
}
