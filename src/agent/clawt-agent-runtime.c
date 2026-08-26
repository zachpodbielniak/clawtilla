/*
 * clawt-agent-runtime.c - How an agent's libreclaw instance is hosted
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "agent/clawt-agent-runtime.h"

/*
 * How much output to keep.
 *
 * The reason anybody wants an agent's log is that it just died, and by then
 * the process owning its stderr is gone -- so it is kept here rather than
 * asked for on demand.  A few hundred lines is enough to see a stack trace
 * or a configuration complaint without holding a running agent's entire
 * output in memory.
 */
#define LOG_RING_LINES 500

/*
 * How long an agent must stay up before a later crash counts as a new
 * problem rather than a continuation of the last one.
 *
 * Comfortably past the backoff cap, so an agent thrashing on startup
 * never clears its streak this way.
 */
#define RESTART_STREAK_RESET_SECONDS 300

#define RESTART_BACKOFF_CAP_SECONDS 300

enum {
    SIGNAL_STARTED,
    SIGNAL_EXITED,
    SIGNAL_LOG_LINE,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

typedef struct {
    ClawtAgentConfig  *config;
    gchar             *agent_id;

    ClawtRestartPolicy restart_policy;
    guint              backoff_seconds;
    guint              max_restarts;
    guint              consecutive_failures;
    gint64             started_at;
    GSource           *restart_source;

    gboolean           stopping;
    gchar             *last_error;

    GQueue            *log_lines;   /* gchar*, oldest first */
} ClawtAgentRuntimePrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE(ClawtAgentRuntime, clawt_agent_runtime,
                                    G_TYPE_OBJECT)

#define PRIV(self) \
    ((ClawtAgentRuntimePrivate *) \
     clawt_agent_runtime_get_instance_private(CLAWT_AGENT_RUNTIME(self)))

gboolean
clawt_agent_runtime_start(ClawtAgentRuntime *self, GError **error)
{
    ClawtAgentRuntimeClass *klass;
    ClawtAgentRuntimePrivate *priv;

    g_return_val_if_fail(CLAWT_IS_AGENT_RUNTIME(self), FALSE);

    priv = PRIV(self);
    klass = CLAWT_AGENT_RUNTIME_GET_CLASS(self);

    if (klass->start == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                            "this runtime cannot be started");
        return FALSE;
    }

    priv->stopping = FALSE;

    if (!klass->start(self, error)) {
        g_clear_pointer(&priv->last_error, g_free);
        if (error != NULL && *error != NULL)
            priv->last_error = g_strdup((*error)->message);
        return FALSE;
    }

    priv->started_at = g_get_monotonic_time();

    g_signal_emit(self, signals[SIGNAL_STARTED], 0);

    return TRUE;
}

void
clawt_agent_runtime_stop(ClawtAgentRuntime *self)
{
    ClawtAgentRuntimeClass *klass;
    ClawtAgentRuntimePrivate *priv;

    g_return_if_fail(CLAWT_IS_AGENT_RUNTIME(self));

    priv = PRIV(self);

    /*
     * Set before stopping, so the exit this causes is not mistaken for a
     * crash and answered with a restart.
     */
    priv->stopping = TRUE;

    if (priv->restart_source != NULL) {
        g_source_destroy(priv->restart_source);
        g_clear_pointer(&priv->restart_source, g_source_unref);
    }

    klass = CLAWT_AGENT_RUNTIME_GET_CLASS(self);
    if (klass->stop != NULL)
        klass->stop(self);
}

gboolean
clawt_agent_runtime_is_alive(ClawtAgentRuntime *self)
{
    ClawtAgentRuntimeClass *klass;

    g_return_val_if_fail(CLAWT_IS_AGENT_RUNTIME(self), FALSE);

    klass = CLAWT_AGENT_RUNTIME_GET_CLASS(self);

    return (klass->is_alive != NULL) ? klass->is_alive(self) : FALSE;
}

GPid
clawt_agent_runtime_get_pid(ClawtAgentRuntime *self)
{
    ClawtAgentRuntimeClass *klass;

    g_return_val_if_fail(CLAWT_IS_AGENT_RUNTIME(self), 0);

    klass = CLAWT_AGENT_RUNTIME_GET_CLASS(self);

    return (klass->get_pid != NULL) ? klass->get_pid(self) : 0;
}

ClawtAgentCaps
clawt_agent_runtime_get_caps(ClawtAgentRuntime *self)
{
    ClawtAgentRuntimeClass *klass;

    g_return_val_if_fail(CLAWT_IS_AGENT_RUNTIME(self),
                         CLAWT_AGENT_CAPS_NONE);

    klass = CLAWT_AGENT_RUNTIME_GET_CLASS(self);

    return (klass->get_caps != NULL) ? klass->get_caps(self)
                                     : CLAWT_AGENT_CAPS_NONE;
}

const gchar *
clawt_agent_runtime_get_agent_id(ClawtAgentRuntime *self)
{
    g_return_val_if_fail(CLAWT_IS_AGENT_RUNTIME(self), NULL);

    return PRIV(self)->agent_id;
}

ClawtAgentConfig *
clawt_agent_runtime_get_config(ClawtAgentRuntime *self)
{
    g_return_val_if_fail(CLAWT_IS_AGENT_RUNTIME(self), NULL);

    return PRIV(self)->config;
}

void
clawt_agent_runtime_set_restart_policy(ClawtAgentRuntime  *self,
                                       ClawtRestartPolicy  policy,
                                       guint               backoff_seconds,
                                       guint               max_restarts)
{
    ClawtAgentRuntimePrivate *priv;

    g_return_if_fail(CLAWT_IS_AGENT_RUNTIME(self));

    priv = PRIV(self);
    priv->restart_policy = policy;
    priv->backoff_seconds = backoff_seconds;
    priv->max_restarts = max_restarts;
}

ClawtRestartPolicy
clawt_agent_runtime_get_restart_policy(
    ClawtAgentRuntime *self
){
    g_return_val_if_fail(CLAWT_IS_AGENT_RUNTIME(self),
                         CLAWT_RESTART_NEVER);

    return PRIV(self)->restart_policy;
}

const gchar *
clawt_agent_runtime_get_last_error(ClawtAgentRuntime *self)
{
    g_return_val_if_fail(CLAWT_IS_AGENT_RUNTIME(self), NULL);

    return PRIV(self)->last_error;
}

void
clawt_agent_runtime_record_log_line(ClawtAgentRuntime *self,
                                    const gchar       *line)
{
    ClawtAgentRuntimePrivate *priv;

    g_return_if_fail(CLAWT_IS_AGENT_RUNTIME(self));
    g_return_if_fail(line != NULL);

    priv = PRIV(self);

    /*
     * Redacted on the way in, not on the way out.  These lines are shown in
     * the clients and pasted into bug reports, and a key that reached the
     * buffer would be handed out every time somebody asked for the log.
     */
    g_queue_push_tail(priv->log_lines, clawt_redact_secrets(line));

    while (g_queue_get_length(priv->log_lines) > LOG_RING_LINES)
        g_free(g_queue_pop_head(priv->log_lines));

    g_signal_emit(self, signals[SIGNAL_LOG_LINE], 0, line);
}

GStrv
clawt_agent_runtime_get_log_tail(ClawtAgentRuntime *self, guint max_lines)
{
    ClawtAgentRuntimePrivate *priv;
    GPtrArray *out;
    GList *l;
    guint length;
    guint skip;
    guint index = 0;

    g_return_val_if_fail(CLAWT_IS_AGENT_RUNTIME(self), NULL);

    priv = PRIV(self);
    out = g_ptr_array_new_with_free_func(g_free);

    length = g_queue_get_length(priv->log_lines);
    skip = (max_lines > 0 && length > max_lines) ? length - max_lines : 0;

    for (l = priv->log_lines->head; l != NULL; l = l->next, index++) {
        if (index < skip)
            continue;
        g_ptr_array_add(out, g_strdup(l->data));
    }

    g_ptr_array_add(out, NULL);

    return (GStrv)g_ptr_array_free(out, FALSE);
}

static gboolean
on_restart_timeout(gpointer user_data)
{
    ClawtAgentRuntime *self = user_data;
    ClawtAgentRuntimePrivate *priv = PRIV(self);
    g_autoptr(GError) error = NULL;

    /*
     * The source is finishing; forget it without destroying it, or the
     * cancel path would later destroy something already gone.
     */
    g_clear_pointer(&priv->restart_source, g_source_unref);

    if (priv->stopping)
        return G_SOURCE_REMOVE;

    if (!clawt_agent_runtime_start(self, &error))
        g_warning("agent %s: restart failed: %s", priv->agent_id,
                  error->message);

    return G_SOURCE_REMOVE;
}

void
clawt_agent_runtime_record_exit(ClawtAgentRuntime *self,
                                gboolean           clean,
                                const gchar       *detail)
{
    ClawtAgentRuntimePrivate *priv;
    gboolean should_restart;
    guint delay;

    g_return_if_fail(CLAWT_IS_AGENT_RUNTIME(self));

    priv = PRIV(self);

    if (detail != NULL) {
        g_free(priv->last_error);
        priv->last_error = g_strdup(detail);
    }

    g_signal_emit(self, signals[SIGNAL_EXITED], 0, clean, detail);

    /* A stop we asked for is not a failure to recover from. */
    if (priv->stopping)
        return;

    switch (priv->restart_policy) {
    case CLAWT_RESTART_ALWAYS:
        should_restart = TRUE;
        break;
    case CLAWT_RESTART_ON_FAILURE:
        should_restart = !clean;
        break;
    case CLAWT_RESTART_NEVER:
    default:
        should_restart = FALSE;
        break;
    }

    if (!should_restart)
        return;

    /*
     * An agent that stayed up starts a fresh streak.
     *
     * The counter is meant to be consecutive failures, and under
     * `on-failure` the clean-exit reset below is unreachable -- the policy
     * only restarts when !clean -- so it became a lifetime total.  An
     * agent that crashed twice months ago and has run happily since would
     * be permanently benched by one unrelated crash, with a message
     * claiming it had failed three times "in a row".
     *
     * Uptime is the honest signal: a process that ran for a good while
     * before dying was not failing to start.
     */
    if (priv->started_at > 0 &&
        g_get_monotonic_time() - priv->started_at >
            ((gint64)RESTART_STREAK_RESET_SECONDS * G_USEC_PER_SEC))
        priv->consecutive_failures = 0;

    if (clean) {
        /*
         * A clean exit under `always` is not a failure, so it does not
         * count towards the give-up limit -- otherwise an agent designed to
         * run, finish and be restarted would exhaust its allowance and stop.
         */
        priv->consecutive_failures = 0;
    } else {
        priv->consecutive_failures++;
    }

    /*
     * An agent that fails instantly and is restarted for ever is a busy
     * loop that starves everything else, so there is a limit and it is
     * reported rather than hit silently.
     */
    if (priv->max_restarts > 0 &&
        priv->consecutive_failures > priv->max_restarts) {
        g_warning("agent %s: failed %u times in a row; leaving it stopped. "
                  "Last error: %s",
                  priv->agent_id, priv->consecutive_failures,
                  priv->last_error != NULL ? priv->last_error : "unknown");
        return;
    }

    delay = priv->backoff_seconds > 0 ? priv->backoff_seconds : 1;
    delay <<= MIN(priv->consecutive_failures, 6);
    delay = MIN(delay, RESTART_BACKOFF_CAP_SECONDS);

    g_info("agent %s: restarting in %u second(s)", priv->agent_id, delay);

    /*
     * Thread-default, not the global default: an embedded daemon runs its
     * own context, and a restart scheduled on a context nobody iterates
     * simply never happens -- the agent stays down for ever while the
     * policy says it should have come back.
     */
    priv->restart_source = clawt_timeout_add_seconds(delay,
                                                     on_restart_timeout,
                                                     self);
}

/* ── Object lifecycle ────────────────────────────────────────────── */

static void
clawt_agent_runtime_dispose(GObject *object)
{
    ClawtAgentRuntimePrivate *priv = PRIV(object);

    priv->stopping = TRUE;

    if (priv->restart_source != NULL) {
        g_source_destroy(priv->restart_source);
        g_clear_pointer(&priv->restart_source, g_source_unref);
    }

    g_clear_pointer(&priv->config, clawt_agent_config_unref);

    G_OBJECT_CLASS(clawt_agent_runtime_parent_class)->dispose(object);
}

static void
clawt_agent_runtime_finalize(GObject *object)
{
    ClawtAgentRuntimePrivate *priv = PRIV(object);

    g_clear_pointer(&priv->agent_id, g_free);
    g_clear_pointer(&priv->last_error, g_free);

    if (priv->log_lines != NULL) {
        g_queue_free_full(priv->log_lines, g_free);
        priv->log_lines = NULL;
    }

    G_OBJECT_CLASS(clawt_agent_runtime_parent_class)->finalize(object);
}

static void
clawt_agent_runtime_class_init(ClawtAgentRuntimeClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->dispose = clawt_agent_runtime_dispose;
    object_class->finalize = clawt_agent_runtime_finalize;

    /**
     * ClawtAgentRuntime::started:
     * @self: the runtime
     */
    signals[SIGNAL_STARTED] =
        g_signal_new("started", CLAWT_TYPE_AGENT_RUNTIME, G_SIGNAL_RUN_LAST,
                     0, NULL, NULL, NULL, G_TYPE_NONE, 0);

    /**
     * ClawtAgentRuntime::exited:
     * @self: the runtime
     * @clean: whether it ended without error
     * @detail: (nullable): how it ended
     */
    signals[SIGNAL_EXITED] =
        g_signal_new("exited", CLAWT_TYPE_AGENT_RUNTIME, G_SIGNAL_RUN_LAST,
                     0, NULL, NULL, NULL, G_TYPE_NONE, 2,
                     G_TYPE_BOOLEAN, G_TYPE_STRING);

    /**
     * ClawtAgentRuntime::log-line:
     * @self: the runtime
     * @line: a line the agent wrote
     */
    signals[SIGNAL_LOG_LINE] =
        g_signal_new("log-line", CLAWT_TYPE_AGENT_RUNTIME, G_SIGNAL_RUN_LAST,
                     0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
clawt_agent_runtime_init(ClawtAgentRuntime *self)
{
    ClawtAgentRuntimePrivate *priv = PRIV(self);

    priv->log_lines = g_queue_new();
    priv->restart_policy = CLAWT_RESTART_ON_FAILURE;
    priv->backoff_seconds = 5;
    priv->max_restarts = 10;
}

void
clawt_agent_runtime_bind_config(ClawtAgentRuntime *self,
                                ClawtAgentConfig  *config)
{
    ClawtAgentRuntimePrivate *priv = PRIV(self);

    g_clear_pointer(&priv->config, clawt_agent_config_unref);
    g_clear_pointer(&priv->agent_id, g_free);

    if (config != NULL) {
        priv->config = clawt_agent_config_ref(config);
        priv->agent_id = g_strdup(clawt_agent_config_get_id(config));
    }
}
