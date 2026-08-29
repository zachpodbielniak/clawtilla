/*
 * clawt-agent-trace-recorder.h - Recording what an agent did, as it did it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The recorder that needs nothing: no compositor, no consent flag, no
 * upstream change, and no computer.  clawtilla already sees every MCP
 * tool call an agent makes and every desktop action it is allowed --
 * this attaches to those two existing points rather than adding a third
 * place for a call to be noticed.
 *
 * It watches a **program**, which is what makes it categorically
 * different from the two demonstration recorders: nothing a person types
 * passes through it.  That is why it is the one that is on by default
 * for anybody who asks to teach a task, and why the other two need a
 * grant of their own.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-types.h"
#include "teach/clawt-teach-recorder.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_AGENT_TRACE_RECORDER \
    (clawt_agent_trace_recorder_get_type())

G_DECLARE_FINAL_TYPE(ClawtAgentTraceRecorder, clawt_agent_trace_recorder,
                     CLAWT, AGENT_TRACE_RECORDER, ClawtTeachRecorder)

/**
 * clawt_agent_trace_recorder_new:
 * @id: the recording's id
 * @directory: where the trace and its frames go
 * @agent_id: whose work is being watched
 *
 * Returns: (transfer full) (nullable): the recorder, or %NULL when @id
 *   is not a usable recording id
 */
ClawtAgentTraceRecorder *clawt_agent_trace_recorder_new(
    const gchar *id,
    const gchar *directory,
    const gchar *agent_id);

/**
 * clawt_agent_trace_recorder_note_tool_call:
 * @self: a #ClawtAgentTraceRecorder
 * @tool: the tool the agent called
 * @args: (nullable): its arguments, serialised
 *
 * One MCP tool call.
 *
 * Fed from the daemon's existing tool observer -- the same hook the
 * repeat counter and the turn watchdog read -- rather than from a second
 * place that sees tool calls.  A second place is a second place to
 * forget, and this tree has the scars.
 *
 * `clawtilla_computer_exec` is recorded as a command rather than as a
 * tool call, because that is what it is to whoever reads the trace: the
 * step somebody would repeat is `git push`, not "the agent called a
 * tool whose second argument was git push".
 */
void clawt_agent_trace_recorder_note_tool_call(
    ClawtAgentTraceRecorder *self,
    const gchar             *tool,
    const gchar             *args);

/**
 * clawt_agent_trace_recorder_note_desktop:
 * @self: a #ClawtAgentTraceRecorder
 * @tool: the desktop tool the agent is about to use
 *
 * One thing the agent did to a screen.
 *
 * Desktop tools do not pass through #ClawtMcpTools at all -- they go
 * straight from the agent's own MCP client to a compositor through the
 * relay -- so they arrive here from the control gate the relay already
 * asks before every acting call.
 */
void clawt_agent_trace_recorder_note_desktop(ClawtAgentTraceRecorder *self,
                                             const gchar             *tool);

G_END_DECLS
