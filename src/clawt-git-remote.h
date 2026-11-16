/*
 * clawt-git-remote.h - Which repository a directory is, by its remotes
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The same repository is spelled many ways: git@gitlab.com:me/proj.git,
 * https://gitlab.com/me/proj, ssh://git@gitlab.com:22/me/proj.git/.  A
 * project matched to a directory, and later a VENTURE forge repository
 * matched to a task, needs one spelling to compare -- the canonical key,
 * `host/owner/repo` -- and the list of a checkout's remotes to try, the
 * one called `origin` first.
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/**
 * ClawtGitRemote:
 * @name: the remote's name, e.g. `origin`
 * @url: its fetch URL, as git spells it
 * @canonical: (nullable): clawt_git_remote_canonical() of @url
 *
 * One remote of a checkout (since 0.39.0).
 */
typedef struct {
	gchar *name;
	gchar *url;
	gchar *canonical;
} ClawtGitRemote;

/**
 * clawt_git_remote_free:
 * @remote: (nullable) (transfer full): a #ClawtGitRemote
 */
void clawt_git_remote_free(ClawtGitRemote *remote);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtGitRemote, clawt_git_remote_free)

/**
 * clawt_git_remote_canonical:
 * @url: a remote URL in any form git accepts: scp-like
 *   (`git@host:owner/repo.git`), `ssh://`, `https://`, `http://`,
 *   `git://`, `file://` or a plain path
 *
 * The one spelling two remotes of the same repository share.  For a
 * network remote it is `host/owner/repo`: the host lower-cased, the user,
 * password and port dropped, the path lower-cased (the forges compare
 * owner and repository names without case), and a trailing `.git` and
 * `/` removed.  A local remote is its absolute path with the same tail
 * removed and its case kept.
 *
 * Ports are dropped on purpose: an SSH remote on port 2222 and the
 * forge's HTTPS clone URL name the same repository, and the path is what
 * tells repositories apart.
 *
 * Returns: (transfer full) (nullable): the key, or %NULL when @url is
 *   empty or names no repository
 */
gchar *clawt_git_remote_canonical(const gchar *url);

/**
 * clawt_git_remotes_parse:
 * @output: what `git remote -v` printed
 *
 * The fetch remotes in @output, each once, `origin` first and the rest in
 * the order git listed them.
 *
 * Returns: (transfer full) (element-type ClawtGitRemote): the remotes,
 *   possibly none
 */
GPtrArray *clawt_git_remotes_parse(const gchar *output);

/**
 * clawt_git_remotes_for_dir_async:
 * @directory: a host path inside a git checkout
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called when git has answered
 * @user_data: (closure): passed to @callback
 *
 * Runs `git -C @directory remote -v`, off the main loop.
 */
void clawt_git_remotes_for_dir_async(const gchar         *directory,
                                     GCancellable        *cancellable,
                                     GAsyncReadyCallback  callback,
                                     gpointer             user_data);

/**
 * clawt_git_remotes_for_dir_finish:
 * @result: the #GAsyncResult
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (element-type ClawtGitRemote) (nullable): the
 *   remotes, as clawt_git_remotes_parse() orders them; %NULL with @error
 *   when git could not be run or the directory is not a checkout
 */
GPtrArray *clawt_git_remotes_for_dir_finish(GAsyncResult  *result,
                                            GError       **error);

/**
 * clawt_git_remotes_for_dir:
 * @directory: a host path inside a git checkout
 * @error: (out) (optional): return location for a #GError
 *
 * The blocking form of clawt_git_remotes_for_dir_async(), for the CLI and
 * tests.  The daemon uses the async one.
 *
 * Returns: (transfer full) (element-type ClawtGitRemote) (nullable): the
 *   remotes, or %NULL with @error
 */
GPtrArray *clawt_git_remotes_for_dir(const gchar  *directory,
                                     GError      **error);

G_END_DECLS
