/*
 * clawt-embedded-runtime.c - Running an agent inside the daemon
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "agent/clawt-embedded-runtime.h"

struct _ClawtEmbeddedRuntime {
    ClawtAgentRuntime parent_instance;

    gchar        *config_path;
    GMainContext *main_context;
    LcApp        *app;
    gboolean      running;
};

G_DEFINE_FINAL_TYPE(ClawtEmbeddedRuntime, clawt_embedded_runtime,
                    CLAWT_TYPE_AGENT_RUNTIME)

ClawtEmbeddedRuntime *
clawt_embedded_runtime_new(ClawtAgentConfig *config,
                           const gchar      *config_path,
                           GMainContext     *main_context)
{
    ClawtEmbeddedRuntime *self;

    g_return_val_if_fail(config != NULL, NULL);
    g_return_val_if_fail(config_path != NULL, NULL);

    self = g_object_new(CLAWT_TYPE_EMBEDDED_RUNTIME, NULL);
    self->config_path = g_strdup(config_path);

    clawt_agent_runtime_bind_config(CLAWT_AGENT_RUNTIME(self), config);

    if (main_context != NULL)
        self->main_context = g_main_context_ref(main_context);

    return self;
}

LcApp *
clawt_embedded_runtime_get_app(ClawtEmbeddedRuntime *self)
{
    g_return_val_if_fail(CLAWT_IS_EMBEDDED_RUNTIME(self), NULL);

    return self->app;
}

static gboolean
embedded_runtime_start(ClawtAgentRuntime *runtime, GError **error)
{
    ClawtEmbeddedRuntime *self = CLAWT_EMBEDDED_RUNTIME(runtime);
    g_autoptr(GError) local = NULL;

    if (self->running)
        return TRUE;

    /*
     * The daemon's own PodEngine is not shared here.  Sharing it would be
     * cheaper, but it would also let one agent's container operations
     * cancel another's, and the daemon has no way to tell them apart
     * afterwards.
     */
    self->app = lc_app_new_embedded(self->config_path, self->main_context,
                                    NULL);

    if (self->app == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_RUNTIME_SPAWN,
                    "could not create an embedded agent for %s",
                    clawt_agent_runtime_get_agent_id(runtime));
        return FALSE;
    }

    /*
     * Config errors surface here rather than at construction: libreclaw
     * loads the file during start, not during new().
     */
    if (!lc_app_start_embedded(self->app, &local)) {
        g_clear_object(&self->app);

        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_RUNTIME_SPAWN,
                    "%s could not start: %s",
                    clawt_agent_runtime_get_agent_id(runtime),
                    local != NULL ? local->message : "unknown reason");
        return FALSE;
    }

    self->running = TRUE;

    clawt_agent_runtime_record_log_line(
        runtime, "started in-process (embedded runtime)");

    return TRUE;
}

static void
embedded_runtime_stop(ClawtAgentRuntime *runtime)
{
    ClawtEmbeddedRuntime *self = CLAWT_EMBEDDED_RUNTIME(runtime);

    if (!self->running)
        return;

    self->running = FALSE;

    if (self->app != NULL) {
        lc_app_stop_embedded(self->app);
        g_clear_object(&self->app);
    }

    clawt_agent_runtime_record_exit(runtime, TRUE, "stopped");
}

static gboolean
embedded_runtime_is_alive(ClawtAgentRuntime *runtime)
{
    return CLAWT_EMBEDDED_RUNTIME(runtime)->running;
}

static GPid
embedded_runtime_get_pid(ClawtAgentRuntime *runtime)
{
    (void)runtime;

    /*
     * Zero, not getpid().  Reporting the daemon's own pid would invite a
     * caller to signal it, and killing "the agent" would take the whole
     * fleet with it.
     */
    return 0;
}

/*
 * What running in-process costs.
 *
 * No INTERRUPT: cancelling a turn means signalling a process and there
 * isn't one.  No separate environment or credentials either, which is why
 * the process runtime remains the default.
 */
static ClawtAgentCaps
embedded_runtime_get_caps(ClawtAgentRuntime *runtime)
{
    (void)runtime;

    return CLAWT_AGENT_CAPS_STREAMING;
}

static void
clawt_embedded_runtime_dispose(GObject *object)
{
    ClawtEmbeddedRuntime *self = CLAWT_EMBEDDED_RUNTIME(object);

    if (self->app != NULL) {
        lc_app_stop_embedded(self->app);
        g_clear_object(&self->app);
    }

    self->running = FALSE;

    G_OBJECT_CLASS(clawt_embedded_runtime_parent_class)->dispose(object);
}

static void
clawt_embedded_runtime_finalize(GObject *object)
{
    ClawtEmbeddedRuntime *self = CLAWT_EMBEDDED_RUNTIME(object);

    g_free(self->config_path);
    g_clear_pointer(&self->main_context, g_main_context_unref);

    G_OBJECT_CLASS(clawt_embedded_runtime_parent_class)->finalize(object);
}

static void
clawt_embedded_runtime_class_init(ClawtEmbeddedRuntimeClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    ClawtAgentRuntimeClass *runtime_class = CLAWT_AGENT_RUNTIME_CLASS(klass);

    object_class->dispose = clawt_embedded_runtime_dispose;
    object_class->finalize = clawt_embedded_runtime_finalize;

    runtime_class->start = embedded_runtime_start;
    runtime_class->stop = embedded_runtime_stop;
    runtime_class->is_alive = embedded_runtime_is_alive;
    runtime_class->get_pid = embedded_runtime_get_pid;
    runtime_class->get_caps = embedded_runtime_get_caps;
}

static void
clawt_embedded_runtime_init(ClawtEmbeddedRuntime *self)
{
    self->running = FALSE;
}
