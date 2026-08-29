/*
 * clawt-ssh-computer.h - A machine somebody else already runs
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The only backend clawtilla does not own.  A container, a distrobox and
 * a VM are things it creates, starts and can destroy; an ssh host was
 * running before the fleet existed and will be running after it, and is
 * somebody's build server or somebody's laptop.  Every decision here
 * follows from that:
 *
 *   - clawt_computer_type_has_machine() is %FALSE, so no client offers
 *     Start or Stop and nothing here can power-cycle a machine people
 *     depend on.
 *   - Where to connect is an **alias out of ~/.ssh/config**, never a
 *     hostname with a user and a key beside it.  ProxyJump, IdentityFile,
 *     Port and User already live there, are already right, and are not
 *     clawtilla's to reimplement.
 *   - Host-key checking is never turned off.  An unknown or changed key
 *     is a refusal that names the remedy, because accepting a key for
 *     somebody is a decision only they can make.
 *   - There is no kernel mount to make, so the mount list becomes an
 *     allowlist over remote paths -- exactly what `confine: allowlist`
 *     is on the host backend, through the same #ClawtSandbox.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include "computer/clawt-computer.h"
#include "computer/clawt-sandbox.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_SSH_COMPUTER (clawt_ssh_computer_get_type())

G_DECLARE_FINAL_TYPE(ClawtSshComputer, clawt_ssh_computer,
                     CLAWT, SSH_COMPUTER, ClawtComputer)

/**
 * ClawtSshStatus:
 * @CLAWT_SSH_STATUS_READY: everything answered
 * @CLAWT_SSH_STATUS_NOT_CONFIGURED: no alias was given
 * @CLAWT_SSH_STATUS_UNREACHABLE: nothing answered at the other end
 * @CLAWT_SSH_STATUS_HOST_KEY: the key is unknown or has changed
 * @CLAWT_SSH_STATUS_AUTH_FAILED: the host answered and refused the login
 * @CLAWT_SSH_STATUS_WORKSPACE_MISSING: logged in, but the directory is not there
 * @CLAWT_SSH_STATUS_NOT_READY: nothing has been checked yet
 *
 * One rung of the ladder that answers "why is this not working".
 *
 * A ladder rather than a set of flags, because the useful answer is
 * exactly one sentence.  Four true symptoms -- unreachable, and
 * therefore unauthenticated, and therefore no workspace, and therefore
 * not ready -- sends somebody to check four things when only the first
 * is a cause.  clawt_ssh_status_resolve() takes every observation and
 * returns the earliest rung that is failing.
 *
 * The order is by what has to be true first.  %CLAWT_SSH_STATUS_HOST_KEY
 * sits above the login because it is a different job: the login is a
 * setting, the key is a decision, and only the second has a remedy that
 * has to be carried out by hand.
 */
typedef enum {
    CLAWT_SSH_STATUS_READY = 0,
    CLAWT_SSH_STATUS_NOT_CONFIGURED,
    CLAWT_SSH_STATUS_UNREACHABLE,
    CLAWT_SSH_STATUS_HOST_KEY,
    CLAWT_SSH_STATUS_AUTH_FAILED,
    CLAWT_SSH_STATUS_WORKSPACE_MISSING,
    CLAWT_SSH_STATUS_NOT_READY
} ClawtSshStatus;

/**
 * ClawtSshProbe:
 * @CLAWT_SSH_PROBE_PRESENT: the thing asked about is there
 * @CLAWT_SSH_PROBE_MISSING: it is genuinely not there
 * @CLAWT_SSH_PROBE_TRANSPORT: the question never got a real answer
 *
 * What came back from asking the remote whether something exists.
 *
 * Three outcomes rather than two, and the third is the point.  A link
 * that dropped, a login that expired and a host that is rebooting all
 * produce a failed probe, and reading any of them as "not there" is how
 * a provisioner sets about creating something that already exists.
 * clawt_ssh_classify_probe() therefore answers
 * %CLAWT_SSH_PROBE_TRANSPORT for everything it cannot positively read
 * as absence -- missing is a claim that has to be earned.
 */
typedef enum {
    CLAWT_SSH_PROBE_PRESENT = 0,
    CLAWT_SSH_PROBE_MISSING,
    CLAWT_SSH_PROBE_TRANSPORT
} ClawtSshProbe;

/**
 * clawt_ssh_computer_new:
 * @agent_id: the agent this belongs to
 * @host: the ssh config alias to reach
 *
 * Returns: (transfer full) (nullable): a new #ClawtSshComputer, or %NULL
 *   if @agent_id is missing
 */
ClawtComputer *clawt_ssh_computer_new(const gchar *agent_id,
                                      const gchar *host);

/**
 * clawt_ssh_computer_set_workspace:
 * @self: a #ClawtSshComputer
 * @workspace: (nullable): where the agent works over there
 *
 * The remote working directory, and the root of the allowlist.
 */
void clawt_ssh_computer_set_workspace(ClawtSshComputer *self,
                                      const gchar      *workspace);

/**
 * clawt_ssh_computer_set_shell:
 * @self: a #ClawtSshComputer
 * @shell: (nullable): the remote shell, or %NULL for /bin/sh
 *
 * Which shell runs the command line clawtilla assembles.
 *
 * ssh hands the remote login shell one string whatever we do, so this is
 * not a way to avoid a shell -- it is a way to name one that behaves.  A
 * login shell that is fish or csh reads the `cd X && ...` prologue
 * differently, and naming /bin/sh explicitly is what makes an ssh
 * computer behave the same on every host in a fleet.
 */
void clawt_ssh_computer_set_shell(ClawtSshComputer *self,
                                  const gchar      *shell);

/**
 * clawt_ssh_computer_set_sandbox:
 * @self: a #ClawtSshComputer
 * @sandbox: (transfer none): the allowlist over remote paths
 *
 * Hands over the confinement.
 *
 * It must be one built by clawt_sandbox_new_remote(): a local sandbox
 * resolves its paths with realpath() against *this* machine, which for a
 * path that only exists over there leaves ".." in the string and lets
 * "/srv/work/../../etc" read as being inside "/srv/work".  Passing a
 * local one is refused rather than accepted quietly, because the two are
 * indistinguishable from the outside and differ exactly on the case that
 * matters.
 */
void clawt_ssh_computer_set_sandbox(ClawtSshComputer *self,
                                    ClawtSandbox     *sandbox);

/**
 * clawt_ssh_computer_apply_mounts:
 * @self: a #ClawtSshComputer
 *
 * Turns the declared mounts into grants on the sandbox.
 *
 * There is no kernel mount to make across an ssh connection, so a mount
 * here is not a mount -- it is the operator saying "this agent may reach
 * that directory over there", and the only thing that can act on it is
 * the allowlist.  Exactly what the host backend does with its mounts,
 * and for the same reason.
 *
 * The **target** is the grant, not the source: the source names a path
 * on the machine the daemon runs on and means nothing at the far end,
 * while the target is by definition where the directory appears inside
 * the computer -- and the computer is the other machine.
 *
 * Called from provision(), and public so the wire can be tested rather
 * than only the sandbox at the end of it. This tree has repeatedly found
 * a correct mechanism that nothing called.
 */
void clawt_ssh_computer_apply_mounts(ClawtSshComputer *self);

/**
 * clawt_ssh_computer_get_sandbox:
 * @self: a #ClawtSshComputer
 *
 * Returns: (transfer none) (nullable): the allowlist
 */
ClawtSandbox *clawt_ssh_computer_get_sandbox(ClawtSshComputer *self);

/**
 * clawt_ssh_computer_set_connect_timeout:
 * @self: a #ClawtSshComputer
 * @seconds: how long to wait for the connection
 *
 * Bounds the one failure that otherwise has no bound.
 *
 * A host that has dropped off the network accepts nothing and refuses
 * nothing, and ssh will sit out the kernel's TCP timeout waiting.  That
 * turns a turn that should have failed in ten seconds into one that
 * hangs, which is much harder to diagnose and holds a worker thread the
 * whole time.
 */
void clawt_ssh_computer_set_connect_timeout(ClawtSshComputer *self,
                                            guint             seconds);

/**
 * clawt_ssh_computer_set_control_persist:
 * @self: a #ClawtSshComputer
 * @seconds: how long an idle multiplexed connection is kept, or 0 for none
 *
 * Turns connection multiplexing on.
 *
 * Without it every single command pays a full TCP connect, key exchange
 * and authentication -- which over a WAN is most of the time an agent
 * spends running anything.  With it the first command opens a master and
 * every later one is a channel on the connection already there.
 *
 * Zero switches it off, which is what happens when the control socket
 * path will not fit in a `sockaddr_un`; see clawt_ssh_control_path().
 */
void clawt_ssh_computer_set_control_persist(ClawtSshComputer *self,
                                            guint             seconds);

/**
 * clawt_ssh_computer_get_status:
 * @self: a #ClawtSshComputer
 *
 * Returns: the rung of the ladder this computer is on
 */
ClawtSshStatus clawt_ssh_computer_get_status(ClawtSshComputer *self);

/**
 * clawt_ssh_host_is_valid:
 * @host: (nullable): the configured alias
 * @error: (out) (optional): return location for a #GError
 *
 * Whether @host is a name that may be handed to ssh as a destination.
 *
 * `[A-Za-z0-9._-]` and nothing else, and never a leading "-".  The
 * character class keeps a shell metacharacter, a space and an embedded
 * `-o` out of a destination; the leading-hyphen rule is the one that
 * matters, because ssh reads an argument beginning with "-" as an option
 * and an alias called `-oProxyCommand=...` would be a command clawtilla
 * ran on somebody's behalf without ever quoting it.
 *
 * It is deliberately narrower than what ssh_config accepts.  Everything
 * refused here can still be reached by giving it an alias in
 * ~/.ssh/config, which is where the rest of the connection already
 * lives.
 *
 * Returns: %TRUE if @host may be used
 */
gboolean clawt_ssh_host_is_valid(const gchar *host, GError **error);

/**
 * clawt_ssh_control_path:
 * @agent_id: the agent
 * @host: the alias
 * @error: (out) (optional): return location for a #GError
 *
 * Where the multiplexing master's socket goes, or %NULL when it will not
 * fit.
 *
 * `sockaddr_un.sun_path` is 108 bytes and an over-long path does not
 * fail at bind time -- ssh simply never creates the master and every
 * command silently pays a fresh handshake, which reads as the remote
 * being slow.  So the length is checked up front with
 * clawt_check_socket_path() and a refusal here means multiplexing is
 * turned off with a reason rather than turned on and broken.
 *
 * Per (agent, alias): two agents on one host must not share a master, or
 * stopping one closes the other's connection mid-command.
 *
 * Returns: (transfer full) (nullable): the socket path, or %NULL
 */
gchar *clawt_ssh_control_path(const gchar  *agent_id,
                              const gchar  *host,
                              GError      **error);

/**
 * clawt_ssh_computer_build_argv:
 * @self: a #ClawtSshComputer
 * @argv: (array zero-terminated=1): the command to run over there
 * @working_dir: (nullable): the remote directory to run it in
 *
 * The exact ssh command line, so it can be asserted on with no ssh
 * anywhere and no host to reach.
 *
 * Every element of @argv is g_shell_quote()d and the result joined into
 * one word, exactly as the container, distrobox and VM backends do.  ssh
 * concatenates whatever follows the destination and feeds it to the
 * remote login shell, so an unquoted `>` would redirect over there --
 * and the failure would be the bad kind, exiting 0 having written a file
 * nobody asked for.
 *
 * `--` goes before the destination, not after it: ssh joins everything
 * after the destination into the remote command, so `ssh host -- ls`
 * asks the remote shell to run `-- ls`.
 *
 * Returns: (transfer full) (array zero-terminated=1) (nullable): the
 *   command line, or %NULL when no alias is configured
 */
GStrv clawt_ssh_computer_build_argv(ClawtSshComputer    *self,
                                    const gchar * const *argv,
                                    const gchar         *working_dir);

/**
 * clawt_ssh_classify_probe:
 * @exit_status: what the probe exited with
 * @stderr_text: (nullable): what it wrote to stderr
 *
 * Tells a thing that is not there from a question that never arrived.
 *
 * Absence has to be claimed explicitly -- an "no such object", "no such
 * image", "no such container" or "no such file" out of the remote -- and
 * everything else is %CLAWT_SSH_PROBE_TRANSPORT.  ssh's own failures
 * exit 255, which no remote command may use for anything else, and are
 * a transport failure whatever the text says.
 *
 * Failing towards "I could not tell" is the whole point.  A flaky link
 * read as "missing" is how a provisioner comes to create something that
 * already exists, and on a machine clawtilla does not own that is
 * somebody else's data.
 *
 * Returns: what the probe actually established
 */
ClawtSshProbe clawt_ssh_classify_probe(gint         exit_status,
                                       const gchar *stderr_text);

/**
 * clawt_ssh_status_resolve:
 * @configured: an alias was given
 * @reachable: something answered at the other end
 * @host_key_ok: the key is known and unchanged
 * @authenticated: the login was accepted
 * @workspace_present: the working directory is there
 * @ready: everything else has been checked
 *
 * Reduces every observation to the one that has to be fixed first.
 *
 * A pure function of six booleans, so the ordering can be asserted on
 * exhaustively rather than reproduced by arranging six real failures.
 *
 * Returns: the earliest failing rung, or %CLAWT_SSH_STATUS_READY
 */
ClawtSshStatus clawt_ssh_status_resolve(gboolean configured,
                                        gboolean reachable,
                                        gboolean host_key_ok,
                                        gboolean authenticated,
                                        gboolean workspace_present,
                                        gboolean ready);

/**
 * clawt_ssh_status_message:
 * @status: a rung
 * @host: (nullable): the alias, for the message
 * @workspace: (nullable): the remote working directory, for the message
 *
 * One sentence saying what is wrong and what to do about it.
 *
 * %CLAWT_SSH_STATUS_HOST_KEY's is the one that earns its place: the
 * remedy is `ssh <alias> true` at a terminal, by a person, because
 * accepting a host key is a decision and clawtilla passing
 * StrictHostKeyChecking=no would be making it for them.
 *
 * Returns: (transfer full): the message
 */
gchar *clawt_ssh_status_message(ClawtSshStatus  status,
                                const gchar    *host,
                                const gchar    *workspace);

/**
 * clawt_ssh_resolve_binary:
 * @name: "ssh" or "sftp"
 * @error: (out) (optional): return location for a #GError
 *
 * Finds the ssh client, the way clawt-pod-bridge.c finds a module:
 * beside the running binary, then the install location, then PATH.
 *
 * The refusal names all three places it looked and the package that
 * provides it, because "ssh: not found" out of a daemon says nothing
 * about which ssh was wanted or where it was expected.
 *
 * Returns: (transfer full) (nullable): the path, or %NULL
 */
gchar *clawt_ssh_resolve_binary(const gchar *name, GError **error);

/**
 * clawt_ssh_sftp_path_is_safe:
 * @path: (nullable): a path about to go into an sftp batch line
 *
 * Whether @path can be written into an sftp batch command unambiguously.
 *
 * sftp's batch language quotes with double quotes and has no escape
 * inside them, so a path holding a `"`, a backslash or a newline cannot
 * be expressed at all -- and a newline in particular would end the line
 * and start a *second sftp command* of the caller's choosing.  Refused
 * rather than mangled.
 *
 * Returns: %TRUE if @path may be used
 */
gboolean clawt_ssh_sftp_path_is_safe(const gchar *path);

/**
 * clawt_ssh_build_sftp_batch:
 * @command: "get" or "put"
 * @first: the first path
 * @second: the second path
 *
 * The one line handed to `sftp -b -`.
 *
 * Returns: (transfer full) (nullable): the batch line, or %NULL when
 *   either path cannot be written safely
 */
gchar *clawt_ssh_build_sftp_batch(const gchar *command,
                                  const gchar *first,
                                  const gchar *second);

G_END_DECLS
