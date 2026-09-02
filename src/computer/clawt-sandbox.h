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
void clawt_sandbox_set_allow_network(ClawtSandbox *self, gboolean allow);
void clawt_sandbox_set_allow_sudo(ClawtSandbox *self, gboolean allow);

ClawtConfineMode clawt_sandbox_get_mode(ClawtSandbox *self);

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
