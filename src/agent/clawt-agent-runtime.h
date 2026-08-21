/*
 * clawt-agent-runtime.h - How an agent's libreclaw instance is hosted
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Two implementations, and the choice matters:
 *
 *   #ClawtProcessRuntime supervises a real libreclaw child.  Crashes stay
 *   contained, the environment and credentials are genuinely separate, and
 *   the agent can live inside its own container.  This is the default.
 *
 *   #ClawtEmbeddedRuntime runs an LcApp inside the daemon.  Cheaper, and
 *   what an in-process host such as cmacs wants, at the cost of sharing a
 *   fate with every other embedded agent.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <gio/gio.h>

#include "clawt-enums.h"
#include "clawt-types.h"
#include "config/clawt-config.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_AGENT_RUNTIME (clawt_agent_runtime_get_type())

G_DECLARE_DERIVABLE_TYPE(ClawtAgentRuntime, clawt_agent_runtime,
                         CLAWT, AGENT_RUNTIME, GObject)

/**
 * ClawtAgentRuntimeClass:
 * @start: bring the agent up
 * @stop: take it down
 * @is_alive: whether it is running right now
 * @get_pid: its process id, or 0 when there is no process
 * @get_caps: what this hosting arrangement makes possible
 *
 * The vtable a hosting arrangement implements.
 */
struct _ClawtAgentRuntimeClass {
    GObjectClass parent_class;

    gboolean       (*start)     (ClawtAgentRuntime  *self,
                                 GError            **error);
    void           (*stop)      (ClawtAgentRuntime  *self);
    gboolean       (*is_alive)  (ClawtAgentRuntime  *self);
    GPid           (*get_pid)   (ClawtAgentRuntime  *self);
    ClawtAgentCaps (*get_caps)  (ClawtAgentRuntime  *self);

    /*< private >*/
    gpointer _padding[8];
};

/**
 * clawt_agent_runtime_start:
 * @self: a #ClawtAgentRuntime
 * @error: (out) (optional): return location for a #GError
 *
 * Brings the agent up.  Returning %TRUE means it was launched, not that it
 * has connected: the link arrives separately.
 *
 * Returns: %TRUE on success
 */
gboolean clawt_agent_runtime_start(ClawtAgentRuntime  *self,
                                   GError            **error);

/**
 * clawt_agent_runtime_stop:
 * @self: a #ClawtAgentRuntime
 *
 * Takes the agent down, and cancels any pending restart.
 */
void clawt_agent_runtime_stop(ClawtAgentRuntime *self);

gboolean       clawt_agent_runtime_is_alive(ClawtAgentRuntime *self);
GPid           clawt_agent_runtime_get_pid(ClawtAgentRuntime *self);
ClawtAgentCaps clawt_agent_runtime_get_caps(ClawtAgentRuntime *self);

/**
 * clawt_agent_runtime_get_agent_id:
 * @self: a #ClawtAgentRuntime
 *
 * Returns: (transfer none): the agent being hosted
 */
const gchar *clawt_agent_runtime_get_agent_id(ClawtAgentRuntime *self);

/**
 * clawt_agent_runtime_set_restart_policy:
 * @self: a #ClawtAgentRuntime
 * @policy: when to restart
 * @backoff_seconds: base delay, doubling on each consecutive failure
 * @max_restarts: consecutive failures before giving up, or 0 for never
 *
 * A cap on restarts exists because an agent that fails instantly and is
 * restarted for ever is a busy loop that starves everything else.
 */
void clawt_agent_runtime_set_restart_policy(ClawtAgentRuntime  *self,
                                            ClawtRestartPolicy  policy,
                                            guint               backoff_seconds,
                                            guint               max_restarts);

/**
 * clawt_agent_runtime_get_last_error:
 * @self: a #ClawtAgentRuntime
 *
 * Returns: (transfer none) (nullable): why it last failed
 */
const gchar *clawt_agent_runtime_get_last_error(ClawtAgentRuntime *self);

/**
 * clawt_agent_runtime_get_log_tail:
 * @self: a #ClawtAgentRuntime
 * @max_lines: how many lines at most
 *
 * The agent's most recent output.
 *
 * Kept in a ring buffer rather than a file, because the reason anybody
 * wants it is that the agent just died and they want to know why -- and by
 * then the process that owned its stderr is gone.
 *
 * Returns: (transfer full) (array zero-terminated=1): the lines
 */
GStrv clawt_agent_runtime_get_log_tail(ClawtAgentRuntime *self,
                                       guint              max_lines);

/*< protected >*/

/**
 * clawt_agent_runtime_record_log_line:
 * @self: a #ClawtAgentRuntime
 * @line: a line of output
 *
 * For subclasses: adds a line to the ring buffer and emits ::log-line.
 */
void clawt_agent_runtime_record_log_line(ClawtAgentRuntime *self,
                                         const gchar       *line);

/**
 * clawt_agent_runtime_record_exit:
 * @self: a #ClawtAgentRuntime
 * @clean: whether it exited without error
 * @detail: (nullable): how it ended
 *
 * For subclasses: reports that the agent stopped, applying the restart
 * policy.
 */
void clawt_agent_runtime_record_exit(ClawtAgentRuntime *self,
                                     gboolean           clean,
                                     const gchar       *detail);

/**
 * clawt_agent_runtime_get_config:
 * @self: a #ClawtAgentRuntime
 *
 * Returns: (transfer none): the agent's configuration
 */
ClawtAgentConfig *clawt_agent_runtime_get_config(ClawtAgentRuntime *self);

G_END_DECLS
