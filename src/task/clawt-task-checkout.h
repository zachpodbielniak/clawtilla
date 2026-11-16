/*
 * clawt-task-checkout.h - A checkout of its own for each piece of work
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Two agents told to "work in ~/src/proj" work on top of each other, and
 * the operator cannot tell whose change is whose.  A task that names a
 * repository gets a checkout of its own instead, on a branch of its own
 * (`clawtilla/<task>`), in its assignee's workspace -- so the agent's own
 * tools can reach it whatever confines them -- and what it changed is a
 * diff against where it started.
 *
 * Two shapes, by what can see the repository:
 *
 * - A *worktree* (`git worktree add`) for an agent whose tools run on the
 *   host unconfined: cheap, and the branch is in the repository already.
 * - A *clone* (`git clone --local`, hard-linked) for one confined by
 *   bwrap or working in a container or VM: a worktree's `.git` points back
 *   into the repository, which a sandbox that binds only the workspace
 *   cannot follow.  Removing a clone first fetches its branch back into
 *   the repository, so no commit is lost with it.
 *
 * Agents on an ssh host or a remote podman connection have no workspace
 * the host can write into theirs, and get no checkout.
 */

#pragma once

#include <gio/gio.h>

#include "clawt-enums.h"
#include "clawt-types.h"
#include "config/clawt-config.h"

G_BEGIN_DECLS

/**
 * clawt_task_checkout_branch_for:
 * @task_id: a task id
 *
 * Returns: (transfer full): `clawtilla/<task id without its prefix>`
 */
gchar *clawt_task_checkout_branch_for(const gchar *task_id);

/**
 * clawt_task_checkout_create:
 * @repository: a host path inside the repository to check out
 * @base: (nullable): the ref to start from; %NULL for the repository's
 *   `HEAD`
 * @destination: where the checkout goes; must not exist yet
 * @branch: the branch to make, e.g. from clawt_task_checkout_branch_for()
 * @mode: %CLAWT_CHECKOUT_WORKTREE or %CLAWT_CHECKOUT_CLONE
 * @out_base: (out) (transfer full) (optional): the commit @base named,
 *   which every later diff is taken against
 * @error: (out) (optional): return location for a #GError
 *
 * Makes the checkout.  Both shapes are local operations (a clone
 * hard-links objects), so this blocks for a moment rather than a while.
 *
 * Returns: %TRUE when the checkout exists on @branch
 */
gboolean clawt_task_checkout_create(const gchar        *repository,
                                    const gchar        *base,
                                    const gchar        *destination,
                                    const gchar        *branch,
                                    ClawtCheckoutMode   mode,
                                    gchar             **out_base,
                                    GError            **error);

/**
 * clawt_task_checkout_remove:
 * @repository: the repository it was made from
 * @root: the directory @destination must be inside (the workspace's
 *   `tasks/`); anything else is refused rather than removed
 * @destination: the checkout
 * @branch: its branch
 * @mode: how it was made
 * @error: (out) (optional): return location for a #GError
 *
 * Takes the checkout away and keeps the work: a worktree's branch is in
 * the repository already, and a clone's is fetched back into it first
 * (as @branch) -- refused, with the clone left alone, if that fails.  The
 * branch itself is never deleted here.
 *
 * Returns: %TRUE when the checkout is gone
 */
gboolean clawt_task_checkout_remove(const gchar        *repository,
                                    const gchar        *root,
                                    const gchar        *destination,
                                    const gchar        *branch,
                                    ClawtCheckoutMode   mode,
                                    GError            **error);

/**
 * clawt_task_checkout_diff_async:
 * @destination: the checkout, or with @head the repository
 * @base: the commit it started from
 * @head: (nullable): %NULL to diff the checkout as it stands; a branch to
 *   diff `<base> <head>` in @destination instead -- for a checkout that
 *   was removed, whose branch the repository kept
 * @max_bytes: how much diff to return at most
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called with the diff
 * @user_data: (closure): passed to @callback
 *
 * What the work changed: `git diff <base>` -- committed and uncommitted
 * changes to tracked files together -- then the files git does not track
 * yet, by name.  Off the main loop.
 */
void clawt_task_checkout_diff_async(const gchar         *destination,
                                    const gchar         *base,
                                    const gchar         *head,
                                    gsize                max_bytes,
                                    GCancellable        *cancellable,
                                    GAsyncReadyCallback  callback,
                                    gpointer             user_data);

/**
 * clawt_task_checkout_diff_finish:
 * @result: the #GAsyncResult
 * @out_truncated: (out) (optional): whether the diff was cut at max_bytes
 * @out_untracked: (out) (transfer full) (optional) (array zero-terminated=1):
 *   new files git does not track yet
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the diff, possibly empty
 */
gchar *clawt_task_checkout_diff_finish(GAsyncResult   *result,
                                       gboolean       *out_truncated,
                                       gchar        ***out_untracked,
                                       GError        **error);

/**
 * clawt_task_checkout_is_dirty:
 * @destination: the checkout
 *
 * Whether the checkout holds work that is not committed: changed or new
 * files.  Committed work survives clawt_task_checkout_remove() on the
 * branch; this does not, so cleaning up after
 * `orchestration.checkout_keep_days` leaves a dirty checkout to the
 * operator.  An unreadable checkout counts as dirty.
 *
 * Returns: %TRUE when removing it would lose something
 */
gboolean clawt_task_checkout_is_dirty(const gchar *destination);

/**
 * clawt_task_checkout_mode_for:
 * @config: the assignee's configuration
 * @error: (out) (optional): return location for a #GError
 *
 * Which shape of checkout this agent can use: a worktree for tools on the
 * host unconfined, a clone for a sandbox or a container, VM or distrobox,
 * none for an ssh host or a remote podman connection
 * (%CLAWT_ERROR_NOT_SUPPORTED says which).
 *
 * Returns: the shape, or %CLAWT_CHECKOUT_NONE with @error set
 */
ClawtCheckoutMode clawt_task_checkout_mode_for(ClawtAgentConfig  *config,
                                               GError           **error);

/**
 * clawt_task_checkout_root_for:
 * @config: an agent's configuration
 *
 * Returns: (transfer full) (nullable): `<workspace>/tasks`, where its
 *   checkouts go, or %NULL without a workspace
 */
gchar *clawt_task_checkout_root_for(ClawtAgentConfig *config);

/**
 * clawt_task_checkout_open:
 * @task: the task, with its id
 * @assignee: (nullable): who does it; a task without one cannot have one
 * @repository: the repository, an absolute host path (`~` is expanded)
 * @base: (nullable): the ref to start from; %NULL for its `HEAD`
 * @error: (out) (optional): return location for a #GError
 *
 * Makes @task's checkout in @assignee's workspace and records it on the
 * task.  Saving the task is the caller's.
 *
 * Returns: %TRUE when the task has its checkout
 */
gboolean clawt_task_checkout_open(ClawtTask         *task,
                                  ClawtAgentConfig  *assignee,
                                  const gchar       *repository,
                                  const gchar       *base,
                                  GError           **error);

/**
 * clawt_task_checkout_guidance:
 * @task: a task
 *
 * The genuine `[clawtilla]` paragraph that tells its assignee where the
 * checkout is, on which branch, and the rules for it.  Part of the
 * assignment contract (clawt_task_assignment_guidance_for()).
 *
 * Returns: (transfer full): the paragraph, or "" without a checkout
 */
gchar *clawt_task_checkout_guidance(ClawtTask *task);

G_END_DECLS
