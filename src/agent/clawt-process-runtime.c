/*
 * clawt-process-runtime.c - Supervising a libreclaw child process
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"

#include <signal.h>
#if defined(__linux__)
#include <sys/prctl.h>
#elif defined(__FreeBSD__)
#include <sys/procctl.h>
#endif
#include "agent/clawt-process-runtime.h"

#include <string.h>

struct _ClawtProcessRuntime {
    ClawtAgentRuntime parent_instance;

    gchar        *binary;
    gchar        *config_path;
    GHashTable   *environment;

    /*
     * `runtime.confine: bwrap`.  NULL runs the child as it always ran.
     * Set before each start by the daemon, from the configuration as it
     * stands then.
     */
    ClawtSandbox *sandbox;

    /*
     * Whether the child that is running was started in the sandbox.
     * Kept apart from `sandbox`, which may be replaced for the *next*
     * start while this child is still the one bwrap is holding -- and
     * what stop() and interrupt() must do depends on how this one was
     * started, not on how the next one will be.
     */
    gboolean      confined;

    /*
     * The libreclaw process inside the box, once found.  bwrap is the
     * child GSubprocess knows about; libreclaw is one or two levels below
     * it, depending on whether bwrap made itself an init.  Cached and
     * rechecked against the tree before each use, because a pid is only
     * an answer for as long as the process that held it is still there.
     */
    GPid          payload;

    /*
     * `runtime.network`.  Set before each start, with the sandbox:
     * anything but open is only given inside one.  `gate_socket` is the
     * agent's own egress-gate socket (gated only), `broker` the daemon's
     * provider-key broker, and `broker_agent` the id a token is
     * registered under for the child that is running, so its exit
     * revokes it.
     */
    ClawtRuntimeNetwork  network;
    gchar               *gate_socket;
    ClawtEgressBroker   *broker;
    gchar               *broker_agent;
    gchar               *relay;

    GSubprocess      *process;
    GCancellable     *cancellable;
    GDataInputStream *log_stream;
    gboolean          running;

    /*
     * Set only when the child has genuinely gone, which is not the same
     * as `running` -- that one was cleared the instant a SIGTERM was
     * *sent*, so a stop reported success while the process was still
     * there and the next start spawned a second one alongside it.
     */
    gboolean          exited;

    /*
     * The context the child's exit will arrive on.
     *
     * g_subprocess_wait_async() completes on whatever was thread-default
     * when it was called, and stop() has to iterate *that* one -- it used
     * to ask for the thread-default again from a different call stack and
     * get a different answer.  On clawtillad both are the process default
     * and it worked by luck; under an embedded daemon, or from a GTask
     * callback (which pushes its own context), the exit arrived somewhere
     * stop() was not looking, so every clean shutdown waited out the full
     * grace period and then SIGKILLed a child that had already gone.
     */
    GMainContext     *context;

    /*
     * Exits seen for a child this runtime had already let go of.  See
     * on_process_exited(); it is a diagnostic, not a control.
     */
    guint superseded_exits;
};

/*
 * How long a child gets to act on SIGTERM before it is killed, as a
 * number of short polls -- 5 seconds, in 50ms steps.  Long enough for
 * libreclaw to close its channels, short enough that stopping an agent is
 * not something you wait on.
 */
#define STOP_TICK_USEC   (50 * 1000)
#define STOP_GRACE_TICKS (100)

/*
 * How long to wait for a SIGKILLed child to be reaped.  Short, because
 * SIGKILL cannot be caught: anything that outlasts this is in
 * uninterruptible sleep and is not going to be helped by waiting longer.
 */
#define KILL_REAP_TICKS  (40)

G_DEFINE_FINAL_TYPE(ClawtProcessRuntime, clawt_process_runtime,
                    CLAWT_TYPE_AGENT_RUNTIME)

static GPid spawned_pid(ClawtProcessRuntime *self);
static GPid confined_payload(ClawtProcessRuntime *self);

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

void
clawt_process_runtime_set_sandbox(ClawtProcessRuntime *self,
                                  ClawtSandbox        *sandbox)
{
    g_return_if_fail(CLAWT_IS_PROCESS_RUNTIME(self));

    g_clear_object(&self->sandbox);

    if (sandbox != NULL)
        self->sandbox = g_object_ref(sandbox);
}

void
clawt_process_runtime_set_egress(ClawtProcessRuntime *self,
                                 ClawtRuntimeNetwork  network,
                                 const gchar         *gate_socket,
                                 ClawtEgressBroker   *broker)
{
    g_return_if_fail(CLAWT_IS_PROCESS_RUNTIME(self));

    self->network = network;
    g_free(self->gate_socket);
    self->gate_socket = g_strdup(gate_socket);
    g_clear_object(&self->broker);

    if (broker != NULL)
        self->broker = g_object_ref(broker);
}

void
clawt_process_runtime_set_relay_binary(ClawtProcessRuntime *self,
                                       const gchar         *path)
{
    g_return_if_fail(CLAWT_IS_PROCESS_RUNTIME(self));

    g_free(self->relay);
    self->relay = g_strdup(path);
}

/*
 * Stops the broker accepting the token minted for the child that was
 * running.  A token lives exactly as long as its process: one that
 * outlived it would be a key anybody who had read the old environment
 * could still spend.
 */
static void
revoke_broker_token(ClawtProcessRuntime *self)
{
    if (self->broker != NULL && self->broker_agent != NULL)
        clawt_egress_broker_revoke(self->broker, self->broker_agent);

    g_clear_pointer(&self->broker_agent, g_free);
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

    /*
     * Finished first, and for whichever child this turns out to be: an
     * async operation is completed by its own callback, not only on the
     * paths that turn out to be interesting.
     *
     * It reads like zombie avoidance and is not.  A GSubprocess reaps
     * through its own child watch whether or not anything ever waits on
     * it -- checked by letting one exit with no wait outstanding at all
     * and finding no entry left in /proc -- so the early return below
     * strands no process.  Ordering this first is about the operation,
     * not about the child, and a later edit that moves it on the belief
     * that a zombie depends on it will be reasoning from the wrong fact.
     */
    g_subprocess_wait_finish(process, result, &error);

    /*
     * And it is only *our* exit if it is the child we are still holding.
     *
     * One runtime object serves an agent for its whole life and replaces
     * its child underneath itself: process_runtime_start() clears
     * `process` and spawns another, and dispose() drops it while the
     * wait started for it is still outstanding.  So this callback can
     * arrive for a child that stopped being the current one some time
     * ago -- and it took `source` on trust, set `running` and `exited`
     * from it, and reported the exit as though the live child had died.
     *
     * clawt_agent's handler for that signal clears the link and moves
     * the agent to STOPPED.  The result was an agent that was running,
     * connected and idle while every surface said "stopped - exited with
     * status 0": it answered nothing, and work delegated to it failed
     * with "the agent handling this stopped before finishing" while ps
     * showed its process perfectly healthy.
     *
     * The way there in production is a child that will not die promptly.
     * process_runtime_stop() waits KILL_REAP_TICKS for the kill to be
     * observed, warns that the child is "probably in uninterruptible
     * sleep", and returns with `running` cleared -- leaving this wait
     * outstanding while the next start() installs a replacement.  When
     * the kernel finally lets the old one go, this runs.
     *
     * Counted rather than only dropped, because an exit arriving for a
     * child nobody is waiting on is worth being able to see: it is the
     * signature of the stop path giving up, and there was no way to
     * observe it from outside while it was corrupting an agent's state.
     */
    if (process != self->process) {
        self->superseded_exits++;

        g_debug("agent runtime: ignoring the exit of a child this runtime "
                "no longer holds (%u so far)", self->superseded_exits);

        return;
    }

    self->running = FALSE;
    self->exited = TRUE;
    revoke_broker_token(self);

    clean = g_subprocess_get_successful(process);

    if (g_subprocess_get_if_signaled(process))
        detail = g_strdup_printf("killed by signal %d",
                                 g_subprocess_get_term_sig(process));
    else
        detail = g_strdup_printf("exited with status %d",
                                 g_subprocess_get_exit_status(process));

    clawt_agent_runtime_record_exit(CLAWT_AGENT_RUNTIME(self), clean, detail);
}

/*
 * Asks the kernel to signal the child if this process disappears.
 *
 * Runs in the child between fork and exec, so it must be
 * async-signal-safe -- prctl(2) on Linux and procctl(2) on FreeBSD are
 * both plain syscalls.
 *
 * A daemon stopped cleanly stops its agents itself, but one that is
 * killed outright runs no handler at all, and its agents were then
 * reparented to init and left holding the ports, the session directory
 * and the sqlite database of an agent nothing was supervising. The next
 * daemon started a second copy alongside each of them, which is the
 * multi-instance collision libreclaw explicitly cannot survive. Three
 * such orphans accumulated in one afternoon of restarts.
 */
static void
die_with_parent(gpointer user_data)
{
    /*
     * And, for a confined agent, the Landlock scope bwrap cannot apply
     * (clawt_sandbox_enter()): the box keeps the host's network, and with
     * it every abstract unix socket on the machine.
     */
    clawt_sandbox_enter((ClawtSandbox *)user_data);

#if defined(__linux__)
    prctl(PR_SET_PDEATHSIG, SIGTERM);
#elif defined(__FreeBSD__)
    int sig = SIGTERM;

    procctl(P_PID, 0, PROC_PDEATHSIG_CTL, &sig);
#endif
}

/*
 * Where clawtillad is, so the things built beside it can be found.
 *
 * NULL when /proc is not available, which only costs those entries.
 */
static gchar *
executable_dir(void)
{
    g_autofree gchar *exe = g_file_read_link("/proc/self/exe", NULL);

    if (exe == NULL)
        return NULL;

    return g_path_get_dirname(exe);
}

/*
 * The libreclaw to run, best first.
 *
 * PATH used to be the only answer, which meant a fresh clone failed at
 * the first agent start with "not on PATH" -- while the binary sat in
 * deps/libreclaw/build/release, built minutes earlier by the same `make`
 * that produced the daemon doing the complaining. Anybody who had ever
 * installed libreclaw, or set defaults.libreclaw_binary once and
 * forgotten, never saw it.
 *
 * The same fix pod modules already have, and for the same reason: a
 * normal build and a normal install should both work without being
 * configured. `deps/libreclaw/build/release` is where clawtilla's own
 * config.mk points LIBRECLAW_OUTDIR, whatever build type the daemon
 * itself was built as.
 */
static gchar *
resolve_libreclaw(void)
{
    g_autofree gchar *exe_dir = executable_dir();

    if (exe_dir != NULL) {
        /* Installed: /usr/local/bin/clawtillad beside libreclaw. */
        g_autofree gchar *beside = g_build_filename(exe_dir, "libreclaw",
                                                    NULL);

        if (g_file_test(beside, G_FILE_TEST_IS_EXECUTABLE))
            return g_steal_pointer(&beside);

        /* A checkout: build/<type>/clawtillad, two levels under the root. */
        {
            g_autofree gchar *in_tree = g_build_filename(
                exe_dir, "..", "..", "deps", "libreclaw", "build", "release",
                "libreclaw", NULL);

            if (g_file_test(in_tree, G_FILE_TEST_IS_EXECUTABLE))
                return g_canonicalize_filename(in_tree, NULL);
        }
    }

    return g_find_program_in_path("libreclaw");
}

/*
 * clawtilla-relay, the in-box end of a gated network: beside the
 * running binary (installed, or build/<type>/ for the daemon), one level
 * up (build/<type>/tests/ for a test), then PATH.  NULL when none of
 * them has it, which refuses the start -- never a gated agent quietly
 * given the open network instead.
 */
static gchar *
resolve_relay(ClawtProcessRuntime *self)
{
    g_autofree gchar *exe_dir = executable_dir();

    if (self->relay != NULL)
        return g_file_test(self->relay, G_FILE_TEST_IS_EXECUTABLE)
               ? g_strdup(self->relay) : NULL;

    if (exe_dir != NULL) {
        g_autofree gchar *beside = g_build_filename(exe_dir,
                                                    "clawtilla-relay", NULL);
        g_autofree gchar *above = g_build_filename(exe_dir, "..",
                                                   "clawtilla-relay", NULL);

        if (g_file_test(beside, G_FILE_TEST_IS_EXECUTABLE))
            return g_steal_pointer(&beside);

        if (g_file_test(above, G_FILE_TEST_IS_EXECUTABLE))
            return g_canonicalize_filename(above, NULL);
    }

    return g_find_program_in_path("clawtilla-relay");
}

/*
 * Grants a file the box needs that the sandbox builder could not know
 * about, unless the box already reaches it.  /usr is in every box.
 */
static void
grant_file(ClawtSandbox *sandbox, const gchar *path, gboolean writable)
{
    g_autofree gchar *real = clawt_canonicalize_missing(path);

    if (real != NULL && clawt_path_is_within(real, "/usr"))
        return;

    if (!clawt_sandbox_path_is_allowed(sandbox, path))
        clawt_sandbox_add_mount_path(sandbox, path, writable);

    if (!writable && real != NULL && g_strcmp0(real, path) != 0) {
        if (!clawt_sandbox_path_is_allowed(sandbox, real))
            clawt_sandbox_add_mount_path(sandbox, real, FALSE);

        clawt_sandbox_add_symlink(sandbox, path);
    }
}

/*
 * The network a gated or closed agent is given: the broker's token in
 * place of each key its CLI can be pointed away from, the proxy
 * variables for the gate, and the relay that makes both reachable from
 * inside a namespace with nothing but loopback in it.
 *
 * Fills @extra (the environment the child will get) and @relay_argv
 * (what goes before libreclaw's own argv).  Fails rather than give the
 * agent an open network: a missing relay, a broker that is not
 * listening while there are keys to broker, or a token that cannot be
 * minted are all refusals.
 */
static gboolean
prepare_network(ClawtProcessRuntime  *self,
                GHashTable          **extra,
                GPtrArray            *relay_argv,
                GError              **error)
{
    ClawtAgentConfig *config =
        clawt_agent_runtime_get_config(CLAWT_AGENT_RUNTIME(self));
    const gchar *agent_id =
        (config != NULL) ? clawt_agent_config_get_id(config) : NULL;
    const gchar *provider = (config != NULL)
        ? clawt_agent_config_get_string(config, "model.provider") : NULL;
    g_autoptr(ClawtEgressBrokerPlan) plan = NULL;
    g_autofree gchar *token = NULL;
    g_autofree gchar *relay = NULL;
    g_autofree gchar *proxy = NULL;
    GHashTable *environment;
    gboolean brokering;

    relay = resolve_relay(self);

    /*
     * Absolute, because the box starts in the workspace: a relative
     * path that named the relay from here names nothing from there.
     */
    if (relay != NULL && !g_path_is_absolute(relay)) {
        gchar *absolute = g_canonicalize_filename(relay, NULL);

        g_free(relay);
        relay = absolute;
    }

    if (relay == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT,
                            "runtime.network: gated and none need "
                            "clawtilla-relay, which was not found beside "
                            "clawtillad, one directory above it, or on "
                            "PATH; build or install it, or set "
                            "runtime.network: open. The agent is not "
                            "given an open network instead");
        return FALSE;
    }

    if (self->network == CLAWT_RUNTIME_NETWORK_GATED &&
        self->gate_socket == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT,
                            "runtime.network: gated, and the daemon has no "
                            "egress gate for this agent");
        return FALSE;
    }

    /*
     * The plan is made by name first, to learn whether there is
     * anything to broker at all; a token is minted only when there is,
     * and only then must the broker be listening.
     */
    plan = clawt_egress_broker_plan(self->environment, provider,
                                    self->network, NULL);
    brokering = plan->grants->len > 0;
    g_clear_pointer(&plan, clawt_egress_broker_plan_free);

    if (brokering) {
        if (self->broker == NULL || agent_id == NULL ||
            clawt_egress_broker_get_socket_path(self->broker) == NULL) {
            g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT,
                                "this agent has a provider key to broker, "
                                "and the daemon's broker is not listening; "
                                "the key is not handed to the agent "
                                "instead");
            return FALSE;
        }

        token = clawt_egress_broker_mint_token(error);

        if (token == NULL)
            return FALSE;
    }

    plan = clawt_egress_broker_plan(self->environment, provider,
                                    self->network, token);
    environment = g_hash_table_ref(plan->environment);

    if (brokering) {
        revoke_broker_token(self);
        clawt_egress_broker_register(self->broker, agent_id, token,
                                     plan->grants);
        self->broker_agent = g_strdup(agent_id);
    }

    /*
     * Both spellings of each variable, because CLIs disagree about
     * which they read; ours replace whatever an agent's env: said, since
     * the gate is the only route there is.  NO_PROXY keeps the broker,
     * on loopback, from being sent through the proxy -- which would
     * refuse it as this machine.
     */
    if (self->network == CLAWT_RUNTIME_NETWORK_GATED) {
        static const gchar *const names[] = {
            "HTTPS_PROXY", "https_proxy", "HTTP_PROXY", "http_proxy", NULL
        };
        guint i;

        proxy = g_strdup_printf("http://127.0.0.1:%d",
                                CLAWT_EGRESS_PROXY_PORT);

        for (i = 0; names[i] != NULL; i++)
            g_hash_table_insert(environment, g_strdup(names[i]),
                                g_strdup(proxy));
    }

    g_hash_table_insert(environment, g_strdup("NO_PROXY"),
                        g_strdup("127.0.0.1,localhost,::1"));
    g_hash_table_insert(environment, g_strdup("no_proxy"),
                        g_strdup("127.0.0.1,localhost,::1"));

    /* The relay, and what it bridges. */
    grant_file(self->sandbox, relay, FALSE);
    g_ptr_array_add(relay_argv, g_strdup(relay));

    if (self->network == CLAWT_RUNTIME_NETWORK_GATED) {
        grant_file(self->sandbox, self->gate_socket, TRUE);
        g_ptr_array_add(relay_argv, g_strdup("--forward"));
        g_ptr_array_add(relay_argv, g_strdup_printf(
            "127.0.0.1:%d=%s", CLAWT_EGRESS_PROXY_PORT, self->gate_socket));
    }

    if (brokering) {
        const gchar *broker_socket =
            clawt_egress_broker_get_socket_path(self->broker);

        grant_file(self->sandbox, broker_socket, TRUE);
        g_ptr_array_add(relay_argv, g_strdup("--forward"));
        g_ptr_array_add(relay_argv, g_strdup_printf(
            "127.0.0.1:%d=%s", CLAWT_EGRESS_BROKER_PORT, broker_socket));
    }

    g_ptr_array_add(relay_argv, g_strdup("--"));

    *extra = environment;
    return TRUE;
}

gboolean
clawt_process_runtime_prepare(ClawtProcessRuntime  *self,
                              GStrv                *out_argv,
                              GStrv                *out_environment,
                              GError              **error)
{
    g_autofree gchar *binary = NULL;
    g_auto(GStrv) environment = NULL;
    g_autoptr(GHashTable) network_environment = NULL;
    g_autoptr(GPtrArray) relay_argv = NULL;
    const gchar *argv[4];

    g_return_val_if_fail(CLAWT_IS_PROCESS_RUNTIME(self), FALSE);
    g_return_val_if_fail(out_argv != NULL, FALSE);
    g_return_val_if_fail(out_environment != NULL, FALSE);

    *out_argv = NULL;
    *out_environment = NULL;

    binary = (self->binary != NULL)
             ? g_strdup(self->binary)
             : resolve_libreclaw();

    if (binary == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_RUNTIME_SPAWN,
                            "cannot find the libreclaw binary. It was not "
                            "beside clawtillad, not in deps/libreclaw's "
                            "build, and not on PATH. Build it with `make -C "
                            "deps/libreclaw`, install it, or set "
                            "defaults.libreclaw_binary to where it is.");
        return FALSE;
    }

    argv[0] = binary;
    argv[1] = "-c";
    argv[2] = self->config_path;
    argv[3] = NULL;

    /*
     * The network comes first, because it decides the environment: under
     * a gated or closed network the keys the broker holds are replaced
     * before anything is built from them.  Only inside a sandbox -- the
     * validator shadows an agent that asks for one without the other,
     * and this refuses rather than trusting that it did.
     */
    if (self->network != CLAWT_RUNTIME_NETWORK_OPEN) {
        if (self->sandbox == NULL) {
            g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT,
                                "runtime.network is not open, and only the "
                                "sandbox (runtime.confine: bwrap) can "
                                "enforce that; the agent is not given an "
                                "open network instead");
            return FALSE;
        }

        relay_argv = g_ptr_array_new_with_free_func(g_free);

        if (!prepare_network(self, &network_environment, relay_argv, error))
            return FALSE;
    }

    /*
     * The shared allowlist plus whatever the agent's own `env:` block
     * names -- never the daemon's environment wholesale.
     */
    environment = clawt_build_child_environment(
        network_environment != NULL ? network_environment
                                    : self->environment);

    if (self->sandbox == NULL) {
        *out_argv = g_strdupv((gchar **)argv);
        *out_environment = g_steal_pointer(&environment);
        return TRUE;
    }

    {
        g_autofree gchar *bwrap = g_find_program_in_path("bwrap");
        g_autofree gchar *real = clawt_canonicalize_missing(binary);
        GStrv wrapped;

        /*
         * Gone since the configuration was validated.  Refused, not run
         * bare: an agent somebody asked to have confined that comes up
         * unconfined is the worst outcome available, and nothing on any
         * surface would say it had happened.
         */
        if (bwrap == NULL) {
            g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT,
                                "runtime.confine: bwrap needs bubblewrap, "
                                "which is not installed where the daemon "
                                "runs; install it (Fedora: bubblewrap) or "
                                "set runtime.confine: none. The agent is "
                                "not run unconfined instead");
            return FALSE;
        }

        /*
         * libreclaw itself, which in a checkout lives in a build tree
         * under $HOME that the box would otherwise not contain.  Bound
         * here rather than by whoever built the sandbox, because only
         * this function knows which binary it settled on.  Asked of the
         * sandbox's own predicate first, so a second start does not bind
         * it twice; one installed under /usr is already there.
         */
        if (real != NULL && !clawt_path_is_within(real, "/usr")) {
            if (!clawt_sandbox_path_is_allowed(self->sandbox, binary))
                clawt_sandbox_add_mount_path(self->sandbox, binary, FALSE);

            if (!clawt_sandbox_path_is_allowed(self->sandbox, real))
                clawt_sandbox_add_mount_path(self->sandbox, real, FALSE);

            /* The name it is run by, when that is a symlink. */
            clawt_sandbox_add_symlink(self->sandbox, binary);
        }

        if (!clawt_sandbox_check_links(self->sandbox, error))
            return FALSE;

        /*
         * Under a gated or closed network the box runs the relay, which
         * runs libreclaw: the relay owns the loopback ports the proxy
         * variables and the broker's base URL name.
         */
        if (relay_argv != NULL) {
            guint i;

            for (i = 0; argv[i] != NULL; i++)
                g_ptr_array_add(relay_argv, g_strdup(argv[i]));

            g_ptr_array_add(relay_argv, NULL);
            wrapped = clawt_sandbox_wrap_argv(
                self->sandbox, (const gchar * const *)relay_argv->pdata);
        } else {
            wrapped = clawt_sandbox_wrap_argv(self->sandbox, argv);
        }

        /*
         * The resolved bubblewrap, not the bare name: the launcher's
         * PATH is the agent's environment, which is not this process's,
         * and which bwrap that finds is not a question worth leaving
         * open.
         */
        g_free(wrapped[0]);
        wrapped[0] = g_steal_pointer(&bwrap);

        *out_argv = wrapped;
        *out_environment = clawt_sandbox_wrap_environment(
            self->sandbox, (const gchar * const *)environment);
    }

    return TRUE;
}

static gboolean
process_runtime_start(ClawtAgentRuntime *runtime, GError **error)
{
    ClawtProcessRuntime *self = CLAWT_PROCESS_RUNTIME(runtime);
    g_autoptr(GSubprocessLauncher) launcher = NULL;
    g_auto(GStrv) argv = NULL;
    g_auto(GStrv) environment = NULL;

    if (self->running) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_AGENT_STATE,
                            "the agent is already running");
        return FALSE;
    }

    if (!clawt_process_runtime_prepare(self, &argv, &environment, error))
        return FALSE;

    launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                         G_SUBPROCESS_FLAGS_STDERR_MERGE);

    /*
     * The daemon's own environment is discarded rather than extended:
     * the launcher gets exactly what prepare() built, which is the
     * allowlist, the agent's own variables, and under a sandbox the
     * TMPDIR, HOME and XDG_* the sandbox owns.
     */
    g_subprocess_launcher_set_environ(launcher, environment);
    g_subprocess_launcher_set_child_setup(launcher, die_with_parent,
                                          self->sandbox, NULL);

    /*
     * The previous one is released first.  Every restart overwrote this
     * field while it still held the last run's GSubprocess, so a
     * long-lived agent that occasionally crashes leaked one per cycle.
     */
    g_clear_object(&self->process);
    self->exited = FALSE;
    self->process = g_subprocess_launcher_spawnv(
        launcher, (const gchar * const *)argv, error);
    if (self->process == NULL) {
        revoke_broker_token(self);
        g_prefix_error(error, "starting %s: ", argv[0]);
        return FALSE;
    }

    self->confined = (self->sandbox != NULL);
    self->payload = 0;
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

    g_clear_pointer(&self->context, g_main_context_unref);
    self->context = g_main_context_ref_thread_default();

    g_subprocess_wait_async(self->process, NULL, on_process_exited,
                            g_object_ref(self));

    return TRUE;
}

guint
clawt_process_runtime_get_superseded_exits(ClawtProcessRuntime *self)
{
    g_return_val_if_fail(CLAWT_IS_PROCESS_RUNTIME(self), 0);

    return self->superseded_exits;
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
     *
     * Under a sandbox it goes to libreclaw inside, not to bwrap: bwrap
     * forwards nothing, and killing it takes libreclaw down with SIGKILL
     * through --die-with-parent.  bwrap exits when its child does, so
     * the wait below still sees the exit it is waiting for.
     */
    {
        GPid payload = self->confined ? confined_payload(self) : 0;

        if (payload > 0 &&
            clawt_process_is_descendant_of(payload, spawned_pid(self)))
            kill(payload, SIGTERM);
        else
            g_subprocess_send_signal(self->process, SIGTERM);
    }

    /*
     * ...and then wait for it, which this did not do.
     *
     * `running` used to be cleared here, the instant the signal was
     * *sent*. So a restart -- which is a stop immediately followed by a
     * start -- found the runtime claiming to be stopped while the child
     * was still shutting down, and spawned a second libreclaw against the
     * same config: same ports, same session directory, same database.
     * The new one exited straight away and the daemon reported the agent
     * "stopped - exited with status 0" while the original went on running,
     * now tracked by nothing.
     *
     * The context is iterated rather than slept through, because the exit
     * arrives on it -- a plain sleep would wait the full grace period
     * every time and then kill a child that had gone in milliseconds.
     */
    {
        GMainContext *context = self->context;
        guint waited;

        if (context == NULL)
            context = g_main_context_default();

        for (waited = 0; waited < STOP_GRACE_TICKS && !self->exited;
             waited++) {
            g_main_context_iteration(context, FALSE);
            g_usleep(STOP_TICK_USEC);
        }
    }

    /*
     * A child that will not go is killed rather than left.  Leaving it is
     * how the orphans happened, and an agent that ignores SIGTERM is
     * exactly the one that most needs stopping.
     */
    if (!self->exited) {
        GMainContext *context = self->context;
        guint waited;

        g_warning("agent runtime: pid %d did not stop within %d seconds of "
                  "SIGTERM; killing it",
                  (gint)clawt_agent_runtime_get_pid(runtime),
                  STOP_GRACE_TICKS * STOP_TICK_USEC / 1000000);
        g_subprocess_force_exit(self->process);

        if (context == NULL)
            context = g_main_context_default();

        /*
         * And then wait for the kill to be *observed*, not merely sent.
         *
         * SIGKILL ends the process, but until someone reaps it the pid is
         * still there -- kill(pid, 0) succeeds on a zombie -- and
         * `exited` is only set by the wait callback, which runs on this
         * context.  Returning before that is the same shape of lie the
         * SIGTERM path above was fixed for: stop() reporting a child gone
         * while the kernel still has an entry for it.
         *
         * Bounded anyway.  SIGKILL cannot be caught, so this ends almost
         * at once; a child stuck in uninterruptible sleep is the kernel's
         * problem and must not become a hung daemon.
         */
        for (waited = 0; waited < KILL_REAP_TICKS && !self->exited; waited++) {
            g_main_context_iteration(context, FALSE);
            g_usleep(STOP_TICK_USEC);
        }

        if (!self->exited)
            g_warning("agent runtime: pid %d has not been reaped after "
                      "SIGKILL; it is probably in uninterruptible sleep",
                      (gint)clawt_agent_runtime_get_pid(runtime));
    }

    self->running = FALSE;
}

static gboolean
process_runtime_is_alive(ClawtAgentRuntime *runtime)
{
    ClawtProcessRuntime *self = CLAWT_PROCESS_RUNTIME(runtime);

    /*
     * Asked of our own record rather than of GSubprocess.
     * g_subprocess_get_if_exited() may only be called once the wait has
     * returned, so asking it about a child that is still running is not
     * merely wrong, it is undefined.
     */
    return self->running && self->process != NULL && !self->exited;
}

/* The pid GSubprocess started, which under a sandbox is bwrap. */
static GPid
spawned_pid(ClawtProcessRuntime *self)
{
    const gchar *identifier;

    if (self->process == NULL)
        return 0;

    identifier = g_subprocess_get_identifier(self->process);

    return (identifier != NULL) ? (GPid)g_ascii_strtoll(identifier, NULL, 10)
                                : 0;
}

/* Whether @pid is a bwrap, by the name the kernel has for it. */
static gboolean
is_bwrap(GPid pid)
{
    g_autofree gchar *path = g_strdup_printf("/proc/%d/comm", (gint)pid);
    g_autofree gchar *comm = NULL;

    if (!g_file_get_contents(path, &comm, NULL, NULL))
        return FALSE;

    /*
     * clawtilla-relay too: under a gated network it is the process
     * between bwrap and libreclaw, and a stop aimed at it would end the
     * agent without libreclaw's clean shutdown.  Its name is exactly the
     * fifteen characters the kernel keeps.
     */
    return g_strcmp0(g_strstrip(comm), "bwrap") == 0 ||
           g_strcmp0(comm, "clawtilla-relay") == 0;
}

/*
 * The libreclaw process inside a sandbox, or 0 when it cannot be found
 * yet.
 *
 * bwrap does not forward signals.  SIGTERM sent to it kills bwrap, and
 * --die-with-parent then SIGKILLs libreclaw -- so a stop aimed at the
 * pid GSubprocess knows ended every confined agent without the clean
 * shutdown libreclaw gives its channels, and an interrupt walked the
 * tree below bwrap and killed libreclaw itself along with the turn.
 * Both now aim here, at the first process below bwrap that is not
 * another bwrap (the init it makes for --unshare-pid).
 */
static GPid
confined_payload(ClawtProcessRuntime *self)
{
    GPid root = spawned_pid(self);
    g_autoptr(GArray) below = NULL;
    guint i;

    if (root <= 0)
        return 0;

    if (self->payload > 0 &&
        clawt_process_is_descendant_of(self->payload, root))
        return self->payload;

    self->payload = 0;

    /*
     * Pre-order, which clawt_process_descendants() gives reversed: the
     * first non-bwrap in walking order is the shallowest, and that is
     * libreclaw rather than the CLI it spawned.
     */
    below = clawt_process_descendants(root);

    for (i = below->len; i > 0; i--) {
        GPid pid = g_array_index(below, GPid, i - 1);

        if (!is_bwrap(pid)) {
            self->payload = pid;
            break;
        }
    }

    return self->payload;
}

static GPid
process_runtime_get_pid(ClawtAgentRuntime *runtime)
{
    ClawtProcessRuntime *self = CLAWT_PROCESS_RUNTIME(runtime);

    /*
     * The agent's process is libreclaw, wherever it runs.  Reporting
     * bwrap's pid would aim interrupt() at bwrap's tree -- which holds
     * libreclaw -- and point anyone reading /proc/<pid>/environ at the
     * wrong environment.  Until libreclaw has been exec'd there is only
     * bwrap to report.
     */
    if (self->confined && self->running && !self->exited) {
        GPid payload = confined_payload(self);

        if (payload > 0)
            return payload;
    }

    return spawned_pid(self);
}

/*
 * Kills what the agent's libreclaw spawned, and nothing above it.
 *
 * The agent's turn is carried out by an AI CLI that libreclaw spawned,
 * and that CLI spawns whatever the model asked for. All of it is below
 * our own child in the process tree, and none of it is our child -- so
 * this walks /proc rather than using GSubprocess, which only knows about
 * the one process it started.
 *
 * Killing the CLI does not cancel libreclaw's wait on it: GLib's
 * g_subprocess_communicate_*_async() abandons the read when its
 * cancellable fires but leaves the process running, which is exactly why
 * `!stop` was never enough. Coming at it from the process side is the
 * half that actually ends the work; libreclaw then sees its child exit
 * and finishes the turn on its own.
 *
 * SIGTERM first and SIGKILL after a grace period, both re-checking that
 * the pid still descends from our child. A pid read a moment ago may
 * have exited and been reused by then, and what inherits a recycled pid
 * belongs to somebody else -- on a busy machine that is the difference
 * between stopping an agent and killing the operator's editor.
 */
static gboolean
process_runtime_interrupt(ClawtAgentRuntime *runtime, guint *out_killed,
                          GError **error)
{
    ClawtProcessRuntime *self = CLAWT_PROCESS_RUNTIME(runtime);
    g_autoptr(GArray) descendants = NULL;
    GPid root;
    guint signalled = 0;
    guint i;

    root = process_runtime_get_pid(runtime);

    if (root <= 0 || !self->running) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_AGENT_STATE,
                            "the agent is not running, so it has nothing "
                            "in flight to stop");
        return FALSE;
    }

    descendants = clawt_process_descendants(root);

    /*
     * Nothing to kill is a success, not a failure.
     *
     * An agent between turns has no CLI running, and pressing stop then
     * is a person confirming what they wanted rather than a mistake to
     * report. The count says what happened, so a caller that cares can
     * tell the two apart.
     */
    for (i = 0; i < descendants->len; i++) {
        GPid pid = g_array_index(descendants, GPid, i);

        if (!clawt_process_is_descendant_of(pid, root))
            continue;

        /*
         * A zombie is not work in flight.  The CLI that has just exited
         * stays in /proc until libreclaw reaps it, and kill() on it
         * succeeds while doing nothing -- so counting it reported an
         * agent that was already between turns as one that had
         * something stopped, and a second press of the button as a
         * first.  On a loaded machine libreclaw's wait can trail the
         * exit by long enough for an operator to see it.
         */
        if (clawt_process_has_exited(pid))
            continue;

        if (kill(pid, SIGTERM) == 0)
            signalled++;
    }

    if (out_killed != NULL)
        *out_killed = signalled;

    if (signalled > 0) {
        /*
         * A short grace period, then SIGKILL whatever ignored the first
         * signal. Blocking here at all is a deliberate exception: this
         * is the one IPC handler whose whole promise is that the work
         * has stopped by the time it answers, and 200ms is under the
         * threshold at which a button feels unresponsive. An AI CLI that
         * needs longer than that to die is one that is not going to.
         */
        g_usleep(200 * 1000);

        for (i = 0; i < descendants->len; i++) {
            GPid pid = g_array_index(descendants, GPid, i);

            if (clawt_process_is_descendant_of(pid, root))
                kill(pid, SIGKILL);
        }
    }

    g_info("runtime: interrupted %s, signalling %u process(es) below pid %d",
           clawt_agent_runtime_get_agent_id(runtime) != NULL
               ? clawt_agent_runtime_get_agent_id(runtime) : "an agent",
           signalled, (gint)root);

    return TRUE;
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
    g_clear_object(&self->sandbox);
    revoke_broker_token(self);
    g_clear_object(&self->broker);

    G_OBJECT_CLASS(clawt_process_runtime_parent_class)->dispose(object);
}

static void
clawt_process_runtime_finalize(GObject *object)
{
    ClawtProcessRuntime *self = CLAWT_PROCESS_RUNTIME(object);

    g_clear_pointer(&self->binary, g_free);
    g_clear_pointer(&self->config_path, g_free);
    g_clear_pointer(&self->gate_socket, g_free);
    g_clear_pointer(&self->relay, g_free);
    g_clear_pointer(&self->context, g_main_context_unref);

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
    runtime_class->interrupt = process_runtime_interrupt;
}

static void
clawt_process_runtime_init(ClawtProcessRuntime *self)
{
    self->running = FALSE;
}
