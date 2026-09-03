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
    gboolean       (*interrupt) (ClawtAgentRuntime  *self,
                                 guint              *out_killed,
                                 GError            **error);

    /*< private >*/
    gpointer _padding[7];
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
 * clawt_agent_runtime_get_uptime_seconds:
 * @self: a #ClawtAgentRuntime
 *
 * How long the process serving this agent has been up.
 *
 * Seconds rather than the stamp it is derived from, because that stamp
 * is monotonic: it counts from an arbitrary origin this process picked
 * at boot and means nothing to a client on another machine, or to the
 * same machine after a reboot.  A duration survives the trip.
 *
 * Returns: seconds since the current process started, or 0 when there
 *   is none -- a stopped agent has no uptime rather than an uptime of
 *   however long ago its last child happened to start
 */
gint64 clawt_agent_runtime_get_uptime_seconds(ClawtAgentRuntime *self);

/**
 * clawt_agent_runtime_get_restarts:
 * @self: a #ClawtAgentRuntime
 *
 * How many times this runtime has replaced its child.
 *
 * Distinct from the consecutive-failure streak the restart policy keeps,
 * which resets on a clean exit and after a long enough run.  This one
 * only ever climbs, because the question it answers is "has the process
 * under this agent been swapped since I last looked" -- and a runtime
 * that respawns in place is precisely how an agent came to be reported
 * stopped while alive.
 *
 * Returns: 0 for a runtime still serving its first child
 */
guint clawt_agent_runtime_get_restarts(ClawtAgentRuntime *self);

/**
 * clawt_agent_runtime_interrupt:
 * @self: a #ClawtAgentRuntime
 * @out_killed: (out) (optional): how many processes were signalled
 * @error: (out) (optional): return location for a #GError
 *
 * Kills what the agent is running right now, and leaves the agent up.
 *
 * An agent's turn is carried out by an AI CLI its libreclaw spawned, and
 * that CLI spawns whatever the model asked for -- a build, a test run, a
 * search. Interrupting kills that whole tree and nothing above it: the
 * libreclaw process keeps its link, its session and its mailbox, so the
 * agent is idle rather than stopped, and the next message reaches it
 * without a start.
 *
 * That is the difference from clawt_agent_runtime_stop(), which takes
 * the agent down and needs a start afterwards.
 *
 * A runtime that has no such tree refuses and says so, naming its own
 * type. An embedded agent runs its turn inside the daemon, where there
 * is no process to signal that is not the daemon itself. Answering %TRUE
 * from a runtime that killed nothing would report a stopped turn that is
 * still running, which is worse than refusing.
 *
 * Returns: %TRUE if the tree was signalled
 */
gboolean clawt_agent_runtime_interrupt(ClawtAgentRuntime  *self,
                                       guint              *out_killed,
                                       GError            **error);

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
 * clawt_agent_runtime_get_restart_policy:
 * @self: a #ClawtAgentRuntime
 *
 * What this runtime will do when its child exits.
 *
 * The policy was write-only, which is part of why it went stale
 * unnoticed: an agent carrying a policy nobody had asked for since its
 * first start looks exactly like one carrying the right one.  Nothing
 * could observe the difference, so nothing could test it.
 *
 * Returns: the policy in force
 */
ClawtRestartPolicy clawt_agent_runtime_get_restart_policy(
    ClawtAgentRuntime *self);

/**
 * clawt_agent_runtime_get_paused_until:
 * @self: a #ClawtAgentRuntime
 *
 * When this agent's account regains its session allowance, as Unix
 * seconds, or 0 if it is not waiting on one.
 *
 * A session usage limit is not a fault and not a rate limit: the CLI
 * never reaches the API, answers from itself and exits, so every
 * attempt before the stated reset costs nothing and achieves nothing.
 * Thirty-three of them in under four minutes is what this exists to
 * stop.
 *
 * Reported rather than acted on here, because what to do about a pause
 * differs by caller -- the router holds delivery, a client draws it,
 * and the restart policy declines to spend an attempt on it.
 *
 * Returns: the reset time, or 0
 */
gint64 clawt_agent_runtime_get_paused_until(ClawtAgentRuntime *self);

/**
 * clawt_agent_runtime_is_paused:
 * @self: a #ClawtAgentRuntime
 * @now: the current time, as Unix seconds
 *
 * Whether the pause is still in force at @now.
 *
 * @now is a parameter rather than read from the clock so the boundary
 * can be exercised from both sides -- a pause that never expires and a
 * pause that expires immediately are both silent failures, and the
 * second one restores exactly the storm this prevents.
 *
 * Returns: %TRUE while the agent is waiting on its account
 */
gboolean clawt_agent_runtime_is_paused(ClawtAgentRuntime *self, gint64 now);

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

/**
 * clawt_agent_runtime_bind_config:
 * @self: a #ClawtAgentRuntime
 * @config: (transfer none): the agent's configuration
 *
 * For subclasses: records which agent this runtime hosts.  Call it from
 * the constructor, before anything reads the agent id.
 *
 * Declared here rather than forward-declared in each subclass, because it
 * was in two files and the second copy is how a signature change becomes
 * a mismatch nobody notices until it crashes.
 */
void clawt_agent_runtime_bind_config(ClawtAgentRuntime *self,
                                     ClawtAgentConfig  *config);

G_END_DECLS
