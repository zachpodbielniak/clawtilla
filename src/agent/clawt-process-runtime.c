/*
 * clawt-process-runtime.c - Supervising a libreclaw child process
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "agent/clawt-process-runtime.h"

#include <string.h>

/* Declared in clawt-agent-runtime.c; internal to the agent layer. */
void clawt_agent_runtime_bind_config(ClawtAgentRuntime *self,
                                     ClawtAgentConfig  *config);

struct _ClawtProcessRuntime {
    ClawtAgentRuntime parent_instance;

    gchar        *binary;
    gchar        *config_path;
    GHashTable   *environment;

    GSubprocess      *process;
    GCancellable     *cancellable;
    GDataInputStream *log_stream;
    gboolean          running;
};

G_DEFINE_FINAL_TYPE(ClawtProcessRuntime, clawt_process_runtime,
                    CLAWT_TYPE_AGENT_RUNTIME)

/*
 * Variables a child always needs, whatever the agent's own environment
 * says.
 *
 * Everything else is dropped.  Inheriting the daemon's environment would
 * be the easy thing and the wrong one: a stray ANTHROPIC_API_KEY reaching a
 * subscription CLI quietly moves it onto pay-as-you-go billing nobody
 * agreed to, and a stray SSH_AUTH_SOCK hands an agent every key in the
 * user's agent.
 */
static const gchar *const passthrough_env[] = {
    "PATH", "HOME", "USER", "LOGNAME", "SHELL", "LANG", "TERM", "TZ",
    "XDG_RUNTIME_DIR", "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME",
    NULL
};

ClawtProcessRuntime *
clawt_process_runtime_new(ClawtAgentConfig *config, const gchar *config_path)
{
    ClawtProcessRuntime *self;

    g_return_val_if_fail(config != NULL, NULL);
    g_return_val_if_fail(config_path != NULL, NULL);

    self = g_object_new(CLAWT_TYPE_PROCESS_RUNTIME, NULL);
    self->config_path = g_strdup(config_path);

    clawt_agent_runtime_bind_config(CLAWT_AGENT_RUNTIME(self), config);

    return self;
}

void
clawt_process_runtime_set_binary(ClawtProcessRuntime *self, const gchar *path)
{
    g_return_if_fail(CLAWT_IS_PROCESS_RUNTIME(self));

    g_free(self->binary);
    self->binary = g_strdup(path);
}

void
clawt_process_runtime_set_environment(ClawtProcessRuntime *self,
                                      GHashTable          *env)
{
    g_return_if_fail(CLAWT_IS_PROCESS_RUNTIME(self));

    g_clear_pointer(&self->environment, g_hash_table_unref);

    if (env != NULL)
        self->environment = g_hash_table_ref(env);
}

/* ── Reading the child's output ──────────────────────────────────── */

static void read_next_line(ClawtProcessRuntime *self,
                           GDataInputStream    *stream);

static void
on_log_line(GObject *source, GAsyncResult *result, gpointer user_data)
{
    g_autoptr(ClawtProcessRuntime) self = user_data;
    g_autofree gchar *line = NULL;
    g_autoptr(GError) error = NULL;
    gsize length = 0;

    line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(source),
                                                result, &length, &error);

    if (line == NULL)
        return;

    clawt_agent_runtime_record_log_line(CLAWT_AGENT_RUNTIME(self), line);
    read_next_line(self, G_DATA_INPUT_STREAM(source));
}

static void
read_next_line(ClawtProcessRuntime *self, GDataInputStream *stream)
{
    /*
     * Deliberately not gated on `running`.  A short-lived agent exits
     * before its output has been drained, and stopping there loses exactly
     * the last thing it said -- which, when it died complaining about its
     * config, is the only line anybody wanted.  Reading continues until EOF
     * or cancellation.
     */
    if (stream == NULL || g_cancellable_is_cancelled(self->cancellable))
        return;

    /*
     * The read holds a reference, so a runtime released while its child is
     * still writing is not finalized under the callback.
     */
    g_data_input_stream_read_line_async(stream, G_PRIORITY_LOW,
                                        self->cancellable, on_log_line,
                                        g_object_ref(self));
}

static void
on_process_exited(GObject *source, GAsyncResult *result, gpointer user_data)
{
    g_autoptr(ClawtProcessRuntime) self = user_data;
    g_autoptr(GError) error = NULL;
    GSubprocess *process = G_SUBPROCESS(source);
    g_autofree gchar *detail = NULL;
    gboolean clean;

    self->running = FALSE;

    g_subprocess_wait_finish(process, result, &error);

    clean = g_subprocess_get_successful(process);

    if (g_subprocess_get_if_signaled(process))
        detail = g_strdup_printf("killed by signal %d",
                                 g_subprocess_get_term_sig(process));
    else
        detail = g_strdup_printf("exited with status %d",
                                 g_subprocess_get_exit_status(process));

    clawt_agent_runtime_record_exit(CLAWT_AGENT_RUNTIME(self), clean, detail);
}

/* ── Starting ────────────────────────────────────────────────────── */

static void
apply_environment(ClawtProcessRuntime  *self,
                  GSubprocessLauncher  *launcher)
{
    gsize i;

    /*
     * A clean slate, then only what was asked for.  See passthrough_env
     * above for why the daemon's environment is not inherited.
     */
    for (i = 0; passthrough_env[i] != NULL; i++) {
        const gchar *value = g_getenv(passthrough_env[i]);

        if (value != NULL)
            g_subprocess_launcher_setenv(launcher, passthrough_env[i],
                                         value, TRUE);
    }

    if (self->environment == NULL)
        return;

    {
        GHashTableIter iter;
        gpointer key;
        gpointer value;

        g_hash_table_iter_init(&iter, self->environment);
        while (g_hash_table_iter_next(&iter, &key, &value))
            g_subprocess_launcher_setenv(launcher, key, value, TRUE);
    }
}

static gboolean
process_runtime_start(ClawtAgentRuntime *runtime, GError **error)
{
    ClawtProcessRuntime *self = CLAWT_PROCESS_RUNTIME(runtime);
    g_autoptr(GSubprocessLauncher) launcher = NULL;
    g_autofree gchar *binary = NULL;
    const gchar *argv[4];

    if (self->running) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_AGENT_STATE,
                            "the agent is already running");
        return FALSE;
    }

    binary = (self->binary != NULL)
             ? g_strdup(self->binary)
             : g_find_program_in_path("libreclaw");

    if (binary == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_RUNTIME_SPAWN,
                            "the libreclaw binary is not on PATH; install it "
                            "or set the binary path explicitly");
        return FALSE;
    }

    launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                         G_SUBPROCESS_FLAGS_STDERR_MERGE);

    /*
     * The daemon's own environment is discarded rather than extended.
     * g_subprocess_launcher_set_environ with an empty list gives a genuinely
     * clean slate; setenv then adds back exactly what belongs.
     */
    {
        const gchar *empty[] = { NULL };

        g_subprocess_launcher_set_environ(launcher, (gchar **)empty);
    }

    apply_environment(self, launcher);

    argv[0] = binary;
    argv[1] = "-c";
    argv[2] = self->config_path;
    argv[3] = NULL;

    self->process = g_subprocess_launcher_spawnv(launcher, argv, error);
    if (self->process == NULL) {
        g_prefix_error(error, "starting %s: ", binary);
        return FALSE;
    }

    self->running = TRUE;
    g_clear_object(&self->cancellable);
    self->cancellable = g_cancellable_new();

    {
        GInputStream *stdout_pipe =
            g_subprocess_get_stdout_pipe(self->process);

        g_clear_object(&self->log_stream);

        if (stdout_pipe != NULL) {
            /*
             * Held on the runtime rather than scoped to this function: the
             * reads outlive the call, and a stream freed when the block ends
             * takes the child's remaining output with it.
             */
            self->log_stream = g_data_input_stream_new(stdout_pipe);
            g_data_input_stream_set_newline_type(
                self->log_stream, G_DATA_STREAM_NEWLINE_TYPE_ANY);
            read_next_line(self, self->log_stream);
        }
    }

    g_subprocess_wait_async(self->process, NULL, on_process_exited,
                            g_object_ref(self));

    return TRUE;
}

static void
process_runtime_stop(ClawtAgentRuntime *runtime)
{
    ClawtProcessRuntime *self = CLAWT_PROCESS_RUNTIME(runtime);

    /*
     * The reads are NOT cancelled here.  Whatever the child says on its way
     * out -- the reason it is refusing to run, usually -- is the part worth
     * keeping, and cancelling now discards it.  They end on their own at
     * EOF when the process goes.
     */
    if (self->process == NULL)
        return;

    /*
     * SIGTERM, not SIGKILL.  libreclaw closes its channels and says goodbye
     * on SIGTERM, which lets Matrix and the clawtilla daemon see a clean
     * shutdown rather than a dropped connection to time out.
     */
    g_subprocess_send_signal(self->process, SIGTERM);
    self->running = FALSE;
}

static gboolean
process_runtime_is_alive(ClawtAgentRuntime *runtime)
{
    ClawtProcessRuntime *self = CLAWT_PROCESS_RUNTIME(runtime);

    return self->running && self->process != NULL &&
           !g_subprocess_get_if_exited(self->process);
}

static GPid
process_runtime_get_pid(ClawtAgentRuntime *runtime)
{
    ClawtProcessRuntime *self = CLAWT_PROCESS_RUNTIME(runtime);
    const gchar *identifier;

    if (self->process == NULL)
        return 0;

    identifier = g_subprocess_get_identifier(self->process);

    return (identifier != NULL) ? (GPid)g_ascii_strtoll(identifier, NULL, 10)
                                : 0;
}

/*
 * What a separate process adds over and above what every agent has: a turn
 * that can be interrupted, because there is a process to signal.
 *
 * Tools and peer messaging are not listed here -- they come from the link
 * the daemon serves, which does not depend on the runtime.
 */
static ClawtAgentCaps
process_runtime_get_caps(ClawtAgentRuntime *runtime)
{
    (void)runtime;

    return CLAWT_AGENT_CAPS_INTERRUPT | CLAWT_AGENT_CAPS_STREAMING;
}

static void
clawt_process_runtime_dispose(GObject *object)
{
    ClawtProcessRuntime *self = CLAWT_PROCESS_RUNTIME(object);

    self->running = FALSE;

    if (self->cancellable != NULL)
        g_cancellable_cancel(self->cancellable);

    if (self->process != NULL) {
        g_subprocess_send_signal(self->process, SIGTERM);
        g_clear_object(&self->process);
    }

    g_clear_object(&self->cancellable);
    g_clear_object(&self->log_stream);
    g_clear_pointer(&self->environment, g_hash_table_unref);

    G_OBJECT_CLASS(clawt_process_runtime_parent_class)->dispose(object);
}

static void
clawt_process_runtime_finalize(GObject *object)
{
    ClawtProcessRuntime *self = CLAWT_PROCESS_RUNTIME(object);

    g_clear_pointer(&self->binary, g_free);
    g_clear_pointer(&self->config_path, g_free);

    G_OBJECT_CLASS(clawt_process_runtime_parent_class)->finalize(object);
}

static void
clawt_process_runtime_class_init(ClawtProcessRuntimeClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    ClawtAgentRuntimeClass *runtime_class = CLAWT_AGENT_RUNTIME_CLASS(klass);

    object_class->dispose = clawt_process_runtime_dispose;
    object_class->finalize = clawt_process_runtime_finalize;

    runtime_class->start = process_runtime_start;
    runtime_class->stop = process_runtime_stop;
    runtime_class->is_alive = process_runtime_is_alive;
    runtime_class->get_pid = process_runtime_get_pid;
    runtime_class->get_caps = process_runtime_get_caps;
}

static void
clawt_process_runtime_init(ClawtProcessRuntime *self)
{
    self->running = FALSE;
}
