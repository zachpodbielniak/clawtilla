/*
 * clawt-sandbox.h - Deciding what a host command may touch
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Enforces the confinement modes for a host computer.  The important thing
 * to understand about them is what they do NOT do:
 *
 *   workspace and allowlist inspect the command before running it,
 *   canonicalising every path that looks like one and refusing the command
 *   if any escapes.  That closes ".." and symlink tricks together, because
 *   both disappear under realpath().  It does not stop a program that opens
 *   paths itself once it is running -- `python -c 'open("/etc/shadow")'`
 *   passes, because there is no path in the command line to check.
 *
 *   bwrap is the only mode that involves the kernel, and therefore the only
 *   one that constrains what a running program can reach.
 *
 * This is stated plainly here, in docs/security.org, and in the generated
 * configuration, because a confinement mode people believe is stronger than
 * it is, is worse than none.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-enums.h"
#include "clawt-types.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_SANDBOX (clawt_sandbox_get_type())

G_DECLARE_FINAL_TYPE(ClawtSandbox, clawt_sandbox, CLAWT, SANDBOX, GObject)

/**
 * clawt_sandbox_new:
 * @mode: how much of the machine to allow
 * @root: (nullable): the working directory
 *
 * Returns: (transfer full): a new #ClawtSandbox
 */
ClawtSandbox *clawt_sandbox_new(ClawtConfineMode  mode,
                                const gchar      *root);

/**
 * clawt_sandbox_new_remote:
 * @mode: how much of that machine to allow
 * @root: (nullable): the working directory, spelled as it is over there
 *
 * The same rules applied to paths on **another** machine.
 *
 * Every path a remote sandbox is given names a filesystem this process
 * cannot see, so realpath() is not merely unhelpful here -- it is wrong.
 * clawt_canonicalize_missing() resolves what it can and returns the rest
 * verbatim, which for a wholly unknown path is the whole string: ".."
 * survives, "/srv/work/../../etc/shadow" reads as a prefix of
 * "/srv/work" with a separator after it, and the containment test says
 * yes while the remote kernel resolves the "..". A remote sandbox
 * therefore normalises lexically instead, through
 * clawt_normalize_path_lexically().
 *
 * The flag belongs to the constructor rather than to a setter because
 * every stored path is normalised as it is added: a sandbox switched to
 * remote after its root and its allow list were in would keep three
 * paths resolved against this machine and compare them against
 * candidates resolved against another.
 *
 * What it costs is symlinks. They are on the other machine and nothing
 * here can follow them, so a remote symlink pointing out of the allowed
 * tree passes this check. clawt_sandbox_describe() says exactly that,
 * because a boundary somebody believes is stronger than it is, is worse
 * than none -- the same sentence at the top of this file.
 *
 * Returns: (transfer full): a new #ClawtSandbox
 */
ClawtSandbox *clawt_sandbox_new_remote(ClawtConfineMode  mode,
                                       const gchar      *root);

/**
 * clawt_sandbox_is_remote:
 * @self: a #ClawtSandbox
 *
 * Returns: %TRUE if the paths name another machine
 */
gboolean clawt_sandbox_is_remote(ClawtSandbox *self);

void clawt_sandbox_add_allow_path(ClawtSandbox *self, const gchar *path);

/**
 * clawt_sandbox_add_mount_path:
 * @self: a #ClawtSandbox
 * @path: a directory the agent's configuration mounts
 * @writable: %TRUE for `mode: rw`, %FALSE for `mode: ro`
 *
 * Grants a path because the agent's configuration mounts it.
 *
 * Kept apart from clawt_sandbox_add_allow_path() because the two mean
 * different things.  `allow_paths:` is a list that only applies under
 * `confine: allowlist`; a mount is an explicit grant the operator wrote,
 * and it applies in every mode.  On a container that mount is real and
 * the kernel makes it reachable -- a host agent that was refused the same
 * path would be behaving differently from the identical config on another
 * backend, which is the kind of inconsistency that costs an afternoon.
 *
 * Under `confine: bwrap` this also emits the bind, which it did not for
 * a long time: the grant was honoured by the argument check and by
 * nothing else, so the path was described to the agent and absent from
 * the sandbox.  @writable decides `--bind` against `--ro-bind`, because
 * a mount bound wider than it was declared is the direction that costs
 * something.
 */
void clawt_sandbox_add_mount_path(ClawtSandbox *self, const gchar *path,
                                  gboolean writable);
void clawt_sandbox_add_deny_path(ClawtSandbox *self, const gchar *path);

/**
 * clawt_sandbox_add_symlink:
 * @self: a #ClawtSandbox
 * @path: a symlink on this machine, spelled the way a program will be
 *   called by -- ~/.local/bin/claude, say
 *
 * Recreates @path inside a bwrap sandbox as a symlink to where it really
 * points, so the spelling works there too.
 *
 * Every stored path is canonicalised, which is right for a containment
 * test and wrong for a name somebody calls a program by: a bind of the
 * link resolves to a bind of its target, and the link's own spelling is
 * then missing from the box.  An AI CLI installed as a symlink on PATH
 * was bound and could not be found.  The target still has to be granted
 * separately; this adds only the name.  Nothing is emitted when the
 * link's directory is already inside a bind, where the link is already
 * there.
 */
void clawt_sandbox_add_symlink(ClawtSandbox *self, const gchar *path);
void clawt_sandbox_set_allow_network(ClawtSandbox *self, gboolean allow);
void clawt_sandbox_set_allow_sudo(ClawtSandbox *self, gboolean allow);

ClawtConfineMode clawt_sandbox_get_mode(ClawtSandbox *self);

/**
 * clawt_sandbox_set_private_home:
 * @self: a #ClawtSandbox
 * @home: (nullable): the home directory to put an empty tmpfs over, or
 *   %NULL to leave $HOME as it is outside
 *
 * Gives a bwrap sandbox an empty, writable home directory at @home, with
 * whatever the sandbox binds beneath it (an account directory, the
 * workspace) brought back on top.
 *
 * Used for a confined agent's own process, where the AI CLI expects to
 * be able to write dotfiles under $HOME and the real one holds every
 * other credential on the machine.  The tmpfs is emitted before any bind,
 * because bwrap applies its operations in order and a tmpfs mounted after
 * a bind beneath it would cover the bind.
 */
void clawt_sandbox_set_private_home(ClawtSandbox *self, const gchar *home);

/**
 * clawt_sandbox_set_agent_confined:
 * @self: a #ClawtSandbox
 * @confined: whether the agent's own process is in a sandbox of its own
 *
 * Tells clawt_sandbox_describe() whether the agent's own Bash, Read and
 * Write run confined (`runtime.confine: bwrap`), so the description it
 * gives the agent stops saying those tools reach the whole host when they
 * do not.
 */
void clawt_sandbox_set_agent_confined(ClawtSandbox *self, gboolean confined);

/**
 * clawt_sandbox_check_links:
 * @self: a #ClawtSandbox
 * @error: (out) (optional): return location for a #GError
 *
 * Refuses a bwrap sandbox whose read-only file binds or denials name a
 * file with more than one hard link.
 *
 * A bind mount protects a path, not an inode.  A file bound read-only
 * that also has a second name under a writable bind is writable through
 * that name, and a denied file covered with /dev/null is readable through
 * its other one -- so `--ro-bind` and `deny_paths` both said more than
 * they did.  A denied directory is walked (to a bound) for the same
 * reason: covering it with a tmpfs hides its names, not its inodes.
 *
 * Anything under /usr or /etc is skipped: those are root's, and with
 * fs.protected_hardlinks an agent cannot make a second name for them.
 * Always %TRUE for the other modes, which bind nothing.
 *
 * Returns: %TRUE if the sandbox can be built as described
 */
gboolean clawt_sandbox_check_links(ClawtSandbox  *self,
                                   GError       **error);

/**
 * clawt_sandbox_wrap_environment:
 * @self: a #ClawtSandbox
 * @environment: (array zero-terminated=1) (nullable): the environment the
 *   command would otherwise get, as `NAME=value` strings
 *
 * The environment to run a wrapped command with.
 *
 * Under bwrap the sandbox owns TMPDIR, HOME and every XDG_* variable, and
 * whatever @environment says about them is dropped: TMPDIR is the box's
 * private /tmp, HOME is the daemon's own home (empty, under
 * clawt_sandbox_set_private_home()), and the XDG base directories are
 * the daemon's own.  An agent's `env:` replaces allowlisted names
 * (clawt_build_child_environment()), so without this TMPDIR=/some/host/dir
 * put the host directory back over the box's private /tmp the moment
 * anything bound it writable.  Any other mode returns a copy unchanged.
 *
 * Returns: (transfer full) (array zero-terminated=1): the environment
 */
GStrv clawt_sandbox_wrap_environment(ClawtSandbox        *self,
                                     const gchar * const *environment);

/**
 * clawt_sandbox_environment_is_owned:
 * @name: an environment variable name
 *
 * Whether a bwrap sandbox sets @name itself: TMPDIR, HOME and XDG_*.
 *
 * Returns: %TRUE if an agent's `env:` cannot set @name inside a sandbox
 */
gboolean clawt_sandbox_environment_is_owned(const gchar *name);

/**
 * clawt_sandbox_enter:
 * @self: (nullable): the #ClawtSandbox whose box the child is about to run
 *
 * What bubblewrap cannot do, applied to the calling process: a Landlock
 * domain scoped so that connect() to an abstract unix socket made outside
 * it fails with EPERM.
 *
 * Abstract sockets live in the network namespace, so any box that keeps
 * the host's network reached all of them -- an X server that trusts the
 * local user is the one that matters.  Call it from the
 * #GSpawnChildSetupFunc of whatever spawns the output of
 * clawt_sandbox_wrap_argv(), after that call: wrap_argv() decides,
 * in the parent, whether it can be applied (Landlock ABI 6, Linux 6.12,
 * and a bwrap that is not setuid) and says so once when it cannot.
 *
 * Async-signal-safe.  Does nothing for a mode other than bwrap.
 */
void clawt_sandbox_enter(ClawtSandbox *self);

/**
 * clawt_sandbox_can_scope_abstract_sockets:
 *
 * Whether this kernel has Landlock scopes (ABI 6), which is what
 * clawt_sandbox_enter() needs.
 *
 * Returns: %TRUE if abstract unix sockets can be closed off
 */
gboolean clawt_sandbox_can_scope_abstract_sockets(void);

/**
 * clawt_sandbox_is_available:
 * @self: a #ClawtSandbox
 * @error: (out) (optional): return location for a #GError
 *
 * Whether this confinement can actually be applied here.
 *
 * bwrap that is asked for and missing is an error, never a quiet downgrade:
 * an agent the user believes is sandboxed and is not, is the worst of the
 * available outcomes.
 *
 * Returns: %TRUE if the mode can be used
 */
gboolean clawt_sandbox_is_available(ClawtSandbox  *self,
                                    GError       **error);

/**
 * clawt_sandbox_check_argv:
 * @self: a #ClawtSandbox
 * @argv: (array zero-terminated=1): the command
 * @error: (out) (optional): return location for a #GError
 *
 * Refuses a command that reaches outside what the agent may touch, or that
 * tries to escalate privilege.
 *
 * Returns: %TRUE if the command may run
 */
gboolean clawt_sandbox_check_argv(ClawtSandbox        *self,
                                  const gchar * const *argv,
                                  GError             **error);

/**
 * clawt_sandbox_wrap_argv:
 * @self: a #ClawtSandbox
 * @argv: (array zero-terminated=1): the command
 *
 * Returns the command to actually run: under bwrap, the original wrapped in
 * a bubblewrap invocation; otherwise the original unchanged.
 *
 * Returns: (transfer full) (array zero-terminated=1): the command to run
 */
GStrv clawt_sandbox_wrap_argv(ClawtSandbox        *self,
                              const gchar * const *argv);

/**
 * clawt_sandbox_path_is_allowed:
 * @self: a #ClawtSandbox
 * @path: a path to test
 *
 * Whether @path is inside what the agent may touch, after resolving
 * symlinks and "..".
 *
 * Returns: %TRUE if the path is allowed
 */
gboolean clawt_sandbox_path_is_allowed(ClawtSandbox *self,
                                       const gchar  *path);

/**
 * clawt_sandbox_describe:
 * @self: a #ClawtSandbox
 *
 * A description for the agent's prompt: what it can reach and what it
 * cannot, so it does not spend turns discovering the limits by trial.
 *
 * Returns: (transfer full): the description
 */
gchar *clawt_sandbox_describe(ClawtSandbox *self);

G_END_DECLS
