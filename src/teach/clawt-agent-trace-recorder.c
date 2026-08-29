/*
 * clawt-agent-trace-recorder.c - Recording what an agent did, as it did it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "teach/clawt-agent-trace-recorder.h"

#include <string.h>

struct _ClawtAgentTraceRecorder {
    ClawtTeachRecorder parent_instance;
};

G_DEFINE_FINAL_TYPE(ClawtAgentTraceRecorder, clawt_agent_trace_recorder,
                    CLAWT_TYPE_TEACH_RECORDER)

/*
 * What this recorder can and cannot see, said on the trace itself.
 *
 * The reader of a trace is deciding whether the procedure in front of
 * them is the whole procedure. An agent's own `bash`, `read` and `write`
 * run in its CLI and never reach clawtilla, so a task that was mostly
 * shell work leaves a trace that looks thin -- and without this sentence
 * that reads as the agent having done very little.
 */
#define AGENT_TRACE_CAVEAT \
    "This trace holds the calls that passed through clawtilla: its " \
    "orchestration tools, commands run on the agent's computer, and " \
    "desktop actions. The agent's own file and shell tools run inside " \
    "its CLI and are not visible here, so a task done mostly in the " \
    "agent's own shell leaves a shorter trace than the work it did."

ClawtAgentTraceRecorder *
clawt_agent_trace_recorder_new(const gchar *id,
                               const gchar *directory,
                               const gchar *agent_id)
{
    ClawtAgentTraceRecorder *self;
    g_autoptr(ClawtTeachTrace) trace = NULL;

    g_return_val_if_fail(id != NULL, NULL);
    g_return_val_if_fail(directory != NULL, NULL);

    trace = clawt_teach_trace_new(id, CLAWT_TEACH_SOURCE_AGENT);

    if (trace == NULL)
        return NULL;

    clawt_teach_trace_set_directory(trace, directory);
    clawt_teach_trace_set_agent_id(trace, agent_id);

    self = g_object_new(CLAWT_TYPE_AGENT_TRACE_RECORDER, NULL);
    clawt_teach_recorder_adopt_trace(CLAWT_TEACH_RECORDER(self), trace);

    return self;
}

static gboolean
agent_start(ClawtTeachRecorder *self, GError **error)
{
    (void)error;

    /*
     * Nothing to open.
     *
     * The steps arrive from hooks the daemon already runs, so there is
     * no session to establish and no consent to check -- an agent
     * watching itself is not watching a person. The caveat is recorded
     * before the first step so that a recording which captured nothing
     * still says what it would have been able to see.
     */
    clawt_teach_recorder_add_caveat(self, AGENT_TRACE_CAVEAT);

    return TRUE;
}

static gboolean
agent_stop(ClawtTeachRecorder *self, GError **error)
{
    (void)self;
    (void)error;

    /*
     * Also nothing.  The base has already flipped `active`, and the
     * hooks check that before noting anything, so no further step can
     * arrive.
     */
    return TRUE;
}

static gboolean
agent_drain(ClawtTeachRecorder *self, GError **error)
{
    (void)self;
    (void)error;

    /*
     * An implementation, not the base's refusal, and the difference
     * matters.  There is genuinely nothing buffered anywhere: steps are
     * pushed in as they happen rather than pulled out of a compositor's
     * ring. A backend with nothing to do says so itself.
     */
    return TRUE;
}

/* ── Feeding it ──────────────────────────────────────────────────── */

/*
 * The command out of `clawtilla_computer_exec`'s arguments.
 *
 * Read from the serialised arguments rather than plumbed separately,
 * because the observer hook is the one place that sees every call and
 * adding a second signature for one tool would be a second place for a
 * tool to be missed.
 */
static gchar *
exec_command(const gchar *args)
{
    g_autoptr(JsonParser) parser = NULL;
    JsonNode *root;
    JsonObject *object;

    if (args == NULL || *args == '\0')
        return NULL;

    parser = json_parser_new();

    if (!json_parser_load_from_data(parser, args, -1, NULL))
        return NULL;

    root = json_parser_get_root(parser);
    object = (root != NULL && JSON_NODE_HOLDS_OBJECT(root))
             ? json_node_get_object(root) : NULL;

    if (object == NULL || !json_object_has_member(object, "command"))
        return NULL;

    return g_strdup(json_object_get_string_member(object, "command"));
}

void
clawt_agent_trace_recorder_note_tool_call(ClawtAgentTraceRecorder *self,
                                          const gchar             *tool,
                                          const gchar             *args)
{
    ClawtTeachRecorder *base;
    ClawtTeachStep *step;
    g_autofree gchar *command = NULL;

    g_return_if_fail(CLAWT_IS_AGENT_TRACE_RECORDER(self));
    g_return_if_fail(tool != NULL);

    base = CLAWT_TEACH_RECORDER(self);

    if (!clawt_teach_recorder_is_active(base))
        return;

    if (g_strcmp0(tool, "clawtilla_computer_exec") == 0)
        command = exec_command(args);

    if (command != NULL) {
        step = clawt_teach_step_new(CLAWT_TEACH_STEP_EXEC, command);
        clawt_teach_step_set_detail(step, "on the agent's computer");
    } else {
        g_autofree gchar *label = g_strdup_printf("Called %s", tool);

        step = clawt_teach_step_new(CLAWT_TEACH_STEP_TOOL, label);
        clawt_teach_step_set_detail(step, args);
    }

    clawt_teach_recorder_note_step(base, step);
}

void
clawt_agent_trace_recorder_note_desktop(ClawtAgentTraceRecorder *self,
                                        const gchar             *tool)
{
    ClawtTeachRecorder *base;
    ClawtTeachStep *step;
    g_autofree gchar *label = NULL;

    g_return_if_fail(CLAWT_IS_AGENT_TRACE_RECORDER(self));
    g_return_if_fail(tool != NULL);

    base = CLAWT_TEACH_RECORDER(self);

    if (!clawt_teach_recorder_is_active(base))
        return;

    label = g_strdup_printf("Desktop: %s", tool);
    step = clawt_teach_step_new(CLAWT_TEACH_STEP_DESKTOP, label);

    clawt_teach_recorder_note_step(base, step);
}

static void
clawt_agent_trace_recorder_class_init(ClawtAgentTraceRecorderClass *klass)
{
    ClawtTeachRecorderClass *recorder_class =
        CLAWT_TEACH_RECORDER_CLASS(klass);

    recorder_class->start = agent_start;
    recorder_class->stop = agent_stop;
    recorder_class->drain = agent_drain;
}

static void
clawt_agent_trace_recorder_init(ClawtAgentTraceRecorder *self)
{
    (void)self;
}
