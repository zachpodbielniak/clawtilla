/*
 * clawt-teach-trace.c - What was recorded, in order, with its caveats
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "teach/clawt-teach-trace.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <string.h>

struct _ClawtTeachStep {
    ClawtTeachStepKind  kind;
    gchar              *label;
    gchar              *detail;
    gchar              *frame;
    gint64              wall_us;
    gint64              offset_us;
};

struct _ClawtTeachTrace {
    gatomicrefcount   refs;

    gchar            *id;
    ClawtTeachSource  source;
    gchar            *agent_id;
    gchar            *goal;
    gchar            *directory;
    gchar            *stop_reason;

    gint64            started_at;
    gint64            ended_at;

    guint             dropped;
    guint             suppressed;

    GPtrArray        *steps;    /* ClawtTeachStep* */
    GPtrArray        *caveats;  /* gchar* */
};

G_DEFINE_BOXED_TYPE(ClawtTeachStep, clawt_teach_step,
                    clawt_teach_step_copy, clawt_teach_step_free)

G_DEFINE_BOXED_TYPE(ClawtTeachTrace, clawt_teach_trace,
                    clawt_teach_trace_ref, clawt_teach_trace_unref)

/* ── One step ────────────────────────────────────────────────────── */

ClawtTeachStep *
clawt_teach_step_new(ClawtTeachStepKind kind, const gchar *label)
{
    ClawtTeachStep *self = g_new0(ClawtTeachStep, 1);

    self->kind = kind;
    self->label = g_strdup(label != NULL ? label : "");

    return self;
}

ClawtTeachStep *
clawt_teach_step_copy(ClawtTeachStep *self)
{
    ClawtTeachStep *copy;

    g_return_val_if_fail(self != NULL, NULL);

    copy = clawt_teach_step_new(self->kind, self->label);
    copy->detail = g_strdup(self->detail);
    copy->frame = g_strdup(self->frame);
    copy->wall_us = self->wall_us;
    copy->offset_us = self->offset_us;

    return copy;
}

void
clawt_teach_step_free(ClawtTeachStep *self)
{
    if (self == NULL)
        return;

    g_free(self->label);
    g_free(self->detail);
    g_free(self->frame);
    g_free(self);
}

ClawtTeachStepKind
clawt_teach_step_get_kind(ClawtTeachStep *self)
{
    g_return_val_if_fail(self != NULL, CLAWT_TEACH_STEP_NOTE);

    return self->kind;
}

const gchar *
clawt_teach_step_get_label(ClawtTeachStep *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->label;
}

const gchar *
clawt_teach_step_get_detail(ClawtTeachStep *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->detail;
}

const gchar *
clawt_teach_step_get_frame(ClawtTeachStep *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->frame;
}

gint64
clawt_teach_step_get_wall_us(ClawtTeachStep *self)
{
    g_return_val_if_fail(self != NULL, 0);

    return self->wall_us;
}

gint64
clawt_teach_step_get_offset_us(ClawtTeachStep *self)
{
    g_return_val_if_fail(self != NULL, 0);

    return self->offset_us;
}

void
clawt_teach_step_set_detail(ClawtTeachStep *self, const gchar *detail)
{
    g_return_if_fail(self != NULL);

    g_free(self->detail);
    self->detail = g_strdup(detail);
}

void
clawt_teach_step_set_frame(ClawtTeachStep *self, const gchar *frame)
{
    g_return_if_fail(self != NULL);

    /*
     * A bare name, never a path.  A trace directory is copied about --
     * out of a state directory kept in git, into a bug report, onto
     * another machine -- and a stored path resolves on exactly one of
     * those.  Refusing a separator here rather than at the callers is
     * the rule this tree keeps having to re-apply: fix it where the
     * value is set, not where somebody noticed.
     */
    if (frame != NULL && strchr(frame, G_DIR_SEPARATOR) != NULL) {
        g_warning("teach: a frame is named by its file name inside the "
                  "trace directory, not by a path; ignoring '%s'", frame);
        return;
    }

    g_free(self->frame);
    self->frame = g_strdup(frame);
}

void
clawt_teach_step_set_times(ClawtTeachStep *self, gint64 wall_us,
                           gint64 offset_us)
{
    g_return_if_fail(self != NULL);

    self->wall_us = wall_us;
    self->offset_us = offset_us;
}

/* ── The trace ───────────────────────────────────────────────────── */

ClawtTeachTrace *
clawt_teach_trace_new(const gchar *id, ClawtTeachSource source)
{
    ClawtTeachTrace *self;

    g_return_val_if_fail(id != NULL, NULL);

    /*
     * The id becomes a directory name under the state directory, so it
     * is checked here rather than trusted -- the same reason
     * clawt_observer_subscribe() checks an agent id before writing a
     * frame named after it.
     */
    if (!clawt_is_valid_id(id))
        return NULL;

    self = g_new0(ClawtTeachTrace, 1);
    g_atomic_ref_count_init(&self->refs);

    self->id = g_strdup(id);
    self->source = source;
    self->steps = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_teach_step_free);
    self->caveats = g_ptr_array_new_with_free_func(g_free);

    return self;
}

ClawtTeachTrace *
clawt_teach_trace_ref(ClawtTeachTrace *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    g_atomic_ref_count_inc(&self->refs);

    return self;
}

void
clawt_teach_trace_unref(ClawtTeachTrace *self)
{
    if (self == NULL)
        return;

    if (!g_atomic_ref_count_dec(&self->refs))
        return;

    g_free(self->id);
    g_free(self->agent_id);
    g_free(self->goal);
    g_free(self->directory);
    g_free(self->stop_reason);
    g_ptr_array_unref(self->steps);
    g_ptr_array_unref(self->caveats);
    g_free(self);
}

const gchar *
clawt_teach_trace_get_id(ClawtTeachTrace *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->id;
}

ClawtTeachSource
clawt_teach_trace_get_source(ClawtTeachTrace *self)
{
    g_return_val_if_fail(self != NULL, CLAWT_TEACH_SOURCE_AGENT);

    return self->source;
}

const gchar *
clawt_teach_trace_get_agent_id(ClawtTeachTrace *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->agent_id;
}

const gchar *
clawt_teach_trace_get_goal(ClawtTeachTrace *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->goal;
}

const gchar *
clawt_teach_trace_get_directory(ClawtTeachTrace *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->directory;
}

const gchar *
clawt_teach_trace_get_stop_reason(ClawtTeachTrace *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->stop_reason;
}

gint64
clawt_teach_trace_get_started_at(ClawtTeachTrace *self)
{
    g_return_val_if_fail(self != NULL, 0);

    return self->started_at;
}

gint64
clawt_teach_trace_get_ended_at(ClawtTeachTrace *self)
{
    g_return_val_if_fail(self != NULL, 0);

    return self->ended_at;
}

guint
clawt_teach_trace_get_dropped(ClawtTeachTrace *self)
{
    g_return_val_if_fail(self != NULL, 0);

    return self->dropped;
}

guint
clawt_teach_trace_get_suppressed(ClawtTeachTrace *self)
{
    g_return_val_if_fail(self != NULL, 0);

    return self->suppressed;
}

void
clawt_teach_trace_set_agent_id(ClawtTeachTrace *self, const gchar *id)
{
    g_return_if_fail(self != NULL);

    g_free(self->agent_id);
    self->agent_id = g_strdup(id);
}

void
clawt_teach_trace_set_goal(ClawtTeachTrace *self, const gchar *goal)
{
    g_return_if_fail(self != NULL);

    g_free(self->goal);
    self->goal = g_strdup(goal);
}

void
clawt_teach_trace_set_directory(ClawtTeachTrace *self,
                                const gchar     *directory)
{
    g_return_if_fail(self != NULL);

    g_free(self->directory);
    self->directory = g_strdup(directory);
}

void
clawt_teach_trace_set_stop_reason(ClawtTeachTrace *self, const gchar *reason)
{
    g_return_if_fail(self != NULL);

    g_free(self->stop_reason);
    self->stop_reason = g_strdup(reason);
}

void
clawt_teach_trace_set_started_at(ClawtTeachTrace *self, gint64 stamp)
{
    g_return_if_fail(self != NULL);

    self->started_at = stamp;
}

void
clawt_teach_trace_set_ended_at(ClawtTeachTrace *self, gint64 stamp)
{
    g_return_if_fail(self != NULL);

    self->ended_at = stamp;
}

void
clawt_teach_trace_add_dropped(ClawtTeachTrace *self, guint count)
{
    g_return_if_fail(self != NULL);

    self->dropped += count;
}

void
clawt_teach_trace_add_suppressed(ClawtTeachTrace *self, guint count)
{
    g_return_if_fail(self != NULL);

    self->suppressed += count;
}

void
clawt_teach_trace_add_step(ClawtTeachTrace *self, ClawtTeachStep *step)
{
    g_return_if_fail(self != NULL);
    g_return_if_fail(step != NULL);

    g_ptr_array_add(self->steps, step);
}

GPtrArray *
clawt_teach_trace_get_steps(ClawtTeachTrace *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->steps;
}

void
clawt_teach_trace_add_caveat(ClawtTeachTrace *self, const gchar *text)
{
    guint i;

    g_return_if_fail(self != NULL);

    if (text == NULL || *text == '\0')
        return;

    for (i = 0; i < self->caveats->len; i++) {
        if (g_strcmp0(g_ptr_array_index(self->caveats, i), text) == 0)
            return;
    }

    g_ptr_array_add(self->caveats, g_strdup(text));
}

GPtrArray *
clawt_teach_trace_get_caveats(ClawtTeachTrace *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->caveats;
}

guint
clawt_teach_trace_count_frames(ClawtTeachTrace *self)
{
    guint count = 0;
    guint i;

    g_return_val_if_fail(self != NULL, 0);

    for (i = 0; i < self->steps->len; i++) {
        ClawtTeachStep *step = g_ptr_array_index(self->steps, i);

        if (step->frame != NULL)
            count++;
    }

    return count;
}

gchar *
clawt_teach_trace_render(ClawtTeachTrace *self, guint max_steps)
{
    g_autoptr(GString) out = NULL;
    guint shown;
    guint i;

    g_return_val_if_fail(self != NULL, NULL);

    out = g_string_new(NULL);

    g_string_append_printf(out, "Recording %s (%s)\n", self->id,
                           clawt_enum_to_nick(CLAWT_TYPE_TEACH_SOURCE,
                                              (gint)self->source));

    if (self->agent_id != NULL)
        g_string_append_printf(out, "Agent: %s\n", self->agent_id);

    if (self->goal != NULL && *self->goal != '\0')
        g_string_append_printf(out, "What was being taught: %s\n",
                               self->goal);

    if (self->started_at > 0 && self->ended_at > self->started_at)
        g_string_append_printf(out, "Length: %" G_GINT64_FORMAT " seconds\n",
                               (self->ended_at - self->started_at) /
                               G_USEC_PER_SEC);

    g_string_append_printf(out, "Steps: %u", self->steps->len);

    if (self->dropped > 0)
        g_string_append_printf(out, ", %u not recorded (the recording hit "
                                    "its limit)", self->dropped);

    if (self->suppressed > 0)
        g_string_append_printf(out, ", %u withheld while capture was "
                                    "paused", self->suppressed);

    g_string_append(out, "\n");

    for (i = 0; i < self->caveats->len; i++)
        g_string_append_printf(out, "\nCaveat: %s\n",
                               (const gchar *)g_ptr_array_index(
                                   self->caveats, i));

    g_string_append(out, "\nSteps, in order:\n");

    shown = self->steps->len;

    if (max_steps > 0 && shown > max_steps)
        shown = max_steps;

    for (i = 0; i < shown; i++) {
        ClawtTeachStep *step = g_ptr_array_index(self->steps, i);

        g_string_append_printf(out, "%u. [%s] %s",
                               i + 1,
                               clawt_enum_to_nick(CLAWT_TYPE_TEACH_STEP_KIND,
                                                  (gint)step->kind),
                               step->label);

        if (step->detail != NULL && *step->detail != '\0')
            g_string_append_printf(out, "\n   %s", step->detail);

        g_string_append_c(out, '\n');
    }

    /*
     * Said rather than left implicit.  A model handed the first 200 of
     * 900 steps and no note would write a procedure that stops in the
     * middle and read as complete.
     */
    if (shown < self->steps->len)
        g_string_append_printf(out,
                               "\n... and %u more steps, not shown here.\n",
                               self->steps->len - shown);

    return g_string_free(g_steal_pointer(&out), FALSE);
}

/* ── JSON ────────────────────────────────────────────────────────── */

static void
add_string_member(JsonBuilder *builder, const gchar *name,
                  const gchar *value)
{
    json_builder_set_member_name(builder, name);

    if (value != NULL)
        json_builder_add_string_value(builder, value);
    else
        json_builder_add_null_value(builder);
}

JsonNode *
clawt_teach_trace_to_json(ClawtTeachTrace *self, gboolean with_steps)
{
    g_autoptr(JsonBuilder) builder = NULL;
    guint i;

    g_return_val_if_fail(self != NULL, NULL);

    builder = json_builder_new();

    json_builder_begin_object(builder);

    add_string_member(builder, "id", self->id);
    add_string_member(builder, "source",
                      clawt_enum_to_nick(CLAWT_TYPE_TEACH_SOURCE,
                                         (gint)self->source));
    add_string_member(builder, "agent", self->agent_id);
    add_string_member(builder, "goal", self->goal);
    add_string_member(builder, "directory", self->directory);
    add_string_member(builder, "stop_reason", self->stop_reason);

    json_builder_set_member_name(builder, "started_at");
    json_builder_add_int_value(builder, self->started_at);

    json_builder_set_member_name(builder, "ended_at");
    json_builder_add_int_value(builder, self->ended_at);

    json_builder_set_member_name(builder, "dropped");
    json_builder_add_int_value(builder, self->dropped);

    json_builder_set_member_name(builder, "suppressed");
    json_builder_add_int_value(builder, self->suppressed);

    json_builder_set_member_name(builder, "step_count");
    json_builder_add_int_value(builder, self->steps->len);

    json_builder_set_member_name(builder, "frame_count");
    json_builder_add_int_value(builder,
                               clawt_teach_trace_count_frames(self));

    /*
     * The caveats are in every payload, with the steps or without them.
     * A listing that showed a demonstration without saying what its
     * guard does not cover would be the place somebody decided it was
     * safe to share.
     */
    json_builder_set_member_name(builder, "caveats");
    json_builder_begin_array(builder);

    for (i = 0; i < self->caveats->len; i++)
        json_builder_add_string_value(
            builder, g_ptr_array_index(self->caveats, i));

    json_builder_end_array(builder);

    if (with_steps) {
        json_builder_set_member_name(builder, "steps");
        json_builder_begin_array(builder);

        for (i = 0; i < self->steps->len; i++) {
            ClawtTeachStep *step = g_ptr_array_index(self->steps, i);

            json_builder_begin_object(builder);
            add_string_member(builder, "kind",
                              clawt_enum_to_nick(CLAWT_TYPE_TEACH_STEP_KIND,
                                                 (gint)step->kind));
            add_string_member(builder, "label", step->label);
            add_string_member(builder, "detail", step->detail);
            add_string_member(builder, "frame", step->frame);

            json_builder_set_member_name(builder, "wall_us");
            json_builder_add_int_value(builder, step->wall_us);

            json_builder_set_member_name(builder, "offset_us");
            json_builder_add_int_value(builder, step->offset_us);

            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);
    }

    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

static const gchar *
object_string(JsonObject *object, const gchar *name)
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
object_int(JsonObject *object, const gchar *name)
{
    JsonNode *node;

    if (object == NULL || !json_object_has_member(object, name))
        return 0;

    node = json_object_get_member(object, name);

    if (node == NULL || !JSON_NODE_HOLDS_VALUE(node))
        return 0;

    return json_node_get_int(node);
}

ClawtTeachTrace *
clawt_teach_trace_from_json(JsonNode *node, GError **error)
{
    ClawtTeachTrace *trace;
    JsonObject *object;
    JsonArray *array;
    const gchar *id;
    gint source = CLAWT_TEACH_SOURCE_AGENT;
    guint i;

    /*
     * A type check is not a pointer check.  json_node_new(OBJECT)
     * answers JSON_NODE_HOLDS_OBJECT() with TRUE and
     * json_node_get_object() with NULL, which is the shape that reached
     * eleven IPC handlers in this tree before it was noticed.
     */
    object = (node != NULL && JSON_NODE_HOLDS_OBJECT(node))
             ? json_node_get_object(node) : NULL;

    if (object == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "that is not a trace");
        return NULL;
    }

    id = object_string(object, "id");

    if (id == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "the trace has no id");
        return NULL;
    }

    clawt_enum_from_nick(CLAWT_TYPE_TEACH_SOURCE,
                         object_string(object, "source"), &source);

    trace = clawt_teach_trace_new(id, (ClawtTeachSource)source);

    if (trace == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "'%s' is not a usable recording id", id);
        return NULL;
    }

    clawt_teach_trace_set_agent_id(trace, object_string(object, "agent"));
    clawt_teach_trace_set_goal(trace, object_string(object, "goal"));
    clawt_teach_trace_set_directory(trace,
                                    object_string(object, "directory"));
    clawt_teach_trace_set_stop_reason(trace,
                                      object_string(object, "stop_reason"));
    trace->started_at = object_int(object, "started_at");
    trace->ended_at = object_int(object, "ended_at");
    trace->dropped = (guint)object_int(object, "dropped");
    trace->suppressed = (guint)object_int(object, "suppressed");

    array = json_object_has_member(object, "caveats")
            ? json_object_get_array_member(object, "caveats") : NULL;

    for (i = 0; array != NULL && i < json_array_get_length(array); i++)
        clawt_teach_trace_add_caveat(
            trace, json_array_get_string_element(array, i));

    array = json_object_has_member(object, "steps")
            ? json_object_get_array_member(object, "steps") : NULL;

    for (i = 0; array != NULL && i < json_array_get_length(array); i++) {
        JsonObject *entry = json_array_get_object_element(array, i);
        ClawtTeachStep *step;
        gint kind = CLAWT_TEACH_STEP_NOTE;

        if (entry == NULL)
            continue;

        clawt_enum_from_nick(CLAWT_TYPE_TEACH_STEP_KIND,
                             object_string(entry, "kind"), &kind);

        step = clawt_teach_step_new((ClawtTeachStepKind)kind,
                                    object_string(entry, "label"));
        clawt_teach_step_set_detail(step, object_string(entry, "detail"));
        clawt_teach_step_set_frame(step, object_string(entry, "frame"));
        clawt_teach_step_set_times(step, object_int(entry, "wall_us"),
                                   object_int(entry, "offset_us"));

        clawt_teach_trace_add_step(trace, step);
    }

    return trace;
}

gboolean
clawt_teach_trace_save(ClawtTeachTrace *self, GError **error)
{
    g_autoptr(JsonNode) node = NULL;
    g_autoptr(JsonGenerator) generator = NULL;
    g_autofree gchar *path = NULL;
    g_autofree gchar *text = NULL;

    g_return_val_if_fail(self != NULL, FALSE);

    if (self->directory == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "this trace has no directory to be saved into");
        return FALSE;
    }

    if (g_mkdir_with_parents(self->directory, 0700) != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "could not create %s: %s", self->directory,
                    g_strerror(errno));
        return FALSE;
    }

    node = clawt_teach_trace_to_json(self, TRUE);
    generator = json_generator_new();
    json_generator_set_pretty(generator, TRUE);
    json_generator_set_root(generator, node);
    text = json_generator_to_data(generator, NULL);

    path = g_build_filename(self->directory, "trace.json", NULL);

    return g_file_set_contents(path, text, -1, error);
}

ClawtTeachTrace *
clawt_teach_trace_load(const gchar *directory, GError **error)
{
    g_autoptr(JsonParser) parser = NULL;
    g_autofree gchar *path = NULL;
    ClawtTeachTrace *trace;

    g_return_val_if_fail(directory != NULL, NULL);

    path = g_build_filename(directory, "trace.json", NULL);
    parser = json_parser_new();

    if (!json_parser_load_from_file(parser, path, error))
        return NULL;

    trace = clawt_teach_trace_from_json(json_parser_get_root(parser), error);

    if (trace == NULL)
        return NULL;

    /*
     * The directory it was found in wins over the one it remembers.
     *
     * A state directory is moved -- restored from a backup, checked out
     * on another machine -- and a trace that kept insisting on its
     * original path would name frames nothing can open, which reads as
     * the frames having been lost.
     */
    clawt_teach_trace_set_directory(trace, directory);

    return trace;
}
