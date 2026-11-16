/*
 * clawt-task-checkout.c - A checkout of its own for each piece of work
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include <errno.h>
#include <string.h>

#include "clawtilla.h"
#include "task/clawt-task-checkout.h"

gchar *
clawt_task_checkout_branch_for(
	const gchar *task_id
){
	g_return_val_if_fail(task_id != NULL, NULL);

	return g_strconcat("clawtilla/",
	                   g_str_has_prefix(task_id, "task-") ? task_id + 5
	                                                      : task_id,
	                   NULL);
}

/*
 * One git command, run to completion: argv after `git -c
 * safe.directory=<dir> -C <dir>`.  Nothing that could prompt or page,
 * and the user's own hooks and templates stay out of a checkout made for
 * an agent.  Fails with what git said.
 */
static gboolean
run_git(
	const gchar         *directory,
	const gchar * const *args,
	gchar              **out_stdout,
	GError             **error
){
	g_autoptr(GSubprocessLauncher) launcher = NULL;
	g_autoptr(GSubprocess) process = NULL;
	g_autoptr(GPtrArray) argv = g_ptr_array_new();
	g_autofree gchar *safe = g_strdup_printf("safe.directory=%s", directory);
	g_autofree gchar *out = NULL;
	g_autofree gchar *err = NULL;
	guint i;

	g_ptr_array_add(argv, (gpointer)"git");
	g_ptr_array_add(argv, (gpointer)"-c");
	g_ptr_array_add(argv, safe);
	g_ptr_array_add(argv, (gpointer)"-C");
	g_ptr_array_add(argv, (gpointer)directory);
	for (i = 0; args[i] != NULL; i++)
		g_ptr_array_add(argv, (gpointer)args[i]);
	g_ptr_array_add(argv, NULL);

	launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
	                                     G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_subprocess_launcher_setenv(launcher, "GIT_TERMINAL_PROMPT", "0", TRUE);
	g_subprocess_launcher_setenv(launcher, "GIT_PAGER", "cat", TRUE);
	g_subprocess_launcher_setenv(launcher, "LC_ALL", "C", TRUE);

	process = g_subprocess_launcher_spawnv(launcher,
		(const gchar * const *)argv->pdata, error);

	if (process == NULL)
		return FALSE;

	if (!g_subprocess_communicate_utf8(process, NULL, NULL, &out, &err,
	                                   error))
		return FALSE;

	if (!g_subprocess_get_successful(process)) {
		g_autofree gchar *said = g_strstrip(g_strdup(err != NULL ? err : ""));

		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
		            "git %s in %s failed: %s", args[0], directory,
		            *said != '\0' ? said : "no reason given");
		return FALSE;
	}

	if (out_stdout != NULL)
		*out_stdout = g_steal_pointer(&out);

	return TRUE;
}

/* `git rev-parse --verify <ref>^{commit}`: the commit a ref names. */
static gchar *
resolve_commit(
	const gchar  *directory,
	const gchar  *ref,
	GError      **error
){
	g_autofree gchar *spec = g_strdup_printf("%s^{commit}", ref);
	const gchar *args[] = { "rev-parse", "--verify", "--quiet", spec, NULL };
	g_autofree gchar *out = NULL;

	if (!run_git(directory, args, &out, error))
		return NULL;

	return g_strstrip(g_steal_pointer(&out));
}

gboolean
clawt_task_checkout_create(
	const gchar        *repository,
	const gchar        *base,
	const gchar        *destination,
	const gchar        *branch,
	ClawtCheckoutMode   mode,
	gchar             **out_base,
	GError            **error
){
	g_autofree gchar *commit = NULL;
	g_autofree gchar *top = NULL;
	g_autofree gchar *parent = NULL;

	g_return_val_if_fail(repository != NULL, FALSE);
	g_return_val_if_fail(destination != NULL, FALSE);
	g_return_val_if_fail(branch != NULL, FALSE);

	if (g_file_test(destination, G_FILE_TEST_EXISTS)) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		            "%s already exists; a checkout is only ever made fresh",
		            destination);
		return FALSE;
	}

	/*
	 * Where the work starts, pinned to a commit: a branch name moves, and
	 * a diff against a moving base would show other people's work as this
	 * task's.
	 */
	{
		const gchar *args[] = { "rev-parse", "--show-toplevel", NULL };

		if (!run_git(repository, args, &top, error))
			return FALSE;
		g_strstrip(top);
	}

	commit = resolve_commit(top, (base != NULL && *base != '\0') ? base
	                                                             : "HEAD",
	                        error);
	if (commit == NULL)
		return FALSE;

	parent = g_path_get_dirname(destination);
	if (g_mkdir_with_parents(parent, 0700) != 0) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
		            "cannot make %s: %s", parent, g_strerror(errno));
		return FALSE;
	}

	switch (mode) {
	case CLAWT_CHECKOUT_WORKTREE: {
		const gchar *args[] = { "worktree", "add", "-b", branch,
		                        destination, commit, NULL };

		if (!run_git(top, args, NULL, error))
			return FALSE;
		break;
	}

	case CLAWT_CHECKOUT_CLONE: {
		/*
		 * A self-contained repository: --local hard-links the objects,
		 * --no-hardlinks is not wanted (the source is read-only to us
		 * anyway), and the new branch is made in the clone, from the
		 * pinned commit, not from whatever the clone checked out.
		 */
		const gchar *clone[] = { "clone", "--quiet", "--local",
		                         "--no-checkout", top, destination, NULL };
		const gchar *checkout[] = { "checkout", "--quiet", "-b", branch,
		                            commit, NULL };

		if (!run_git(parent, clone, NULL, error))
			return FALSE;

		if (!run_git(destination, checkout, NULL, error))
			return FALSE;
		break;
	}

	case CLAWT_CHECKOUT_NONE:
	default:
		g_set_error_literal(error, CLAWT_ERROR,
		                    CLAWT_ERROR_INVALID_ARGUMENT,
		                    "no checkout shape was chosen");
		return FALSE;
	}

	if (out_base != NULL)
		*out_base = g_steal_pointer(&commit);

	return TRUE;
}

gboolean
clawt_task_checkout_remove(
	const gchar        *repository,
	const gchar        *root,
	const gchar        *destination,
	const gchar        *branch,
	ClawtCheckoutMode   mode,
	GError            **error
){
	g_return_val_if_fail(repository != NULL, FALSE);
	g_return_val_if_fail(root != NULL, FALSE);
	g_return_val_if_fail(destination != NULL, FALSE);
	g_return_val_if_fail(branch != NULL, FALSE);

	if (!clawt_path_is_within(destination, root)) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		            "%s is not inside %s; refusing to remove it",
		            destination, root);
		return FALSE;
	}

	if (!g_file_test(destination, G_FILE_TEST_IS_DIR)) {
		/* Already gone: a worktree's record may still need pruning. */
		if (mode == CLAWT_CHECKOUT_WORKTREE) {
			const gchar *args[] = { "worktree", "prune", NULL };

			run_git(repository, args, NULL, NULL);
		}

		return TRUE;
	}

	if (mode == CLAWT_CHECKOUT_WORKTREE) {
		const gchar *args[] = { "worktree", "remove", "--force",
		                        destination, NULL };

		return run_git(repository, args, NULL, error);
	}

	/*
	 * A clone's commits live only in the clone until they are fetched
	 * back.  `+` because the branch belongs to this task alone and the
	 * clone's copy is the newer one; refused outright if it cannot be
	 * fetched, since then the delete would be the only copy going.
	 */
	{
		g_autofree gchar *refspec = g_strdup_printf("+%s:%s", branch, branch);
		const gchar *fetch[] = { "fetch", "--quiet", destination, refspec,
		                         NULL };
		if (!run_git(repository, fetch, NULL, error))
			return FALSE;

		return clawt_remove_tree(destination, root, error);
	}
}

gboolean
clawt_task_checkout_is_dirty(
	const gchar *destination
){
	const gchar *args[] = { "status", "--porcelain", NULL };
	g_autofree gchar *out = NULL;

	if (!run_git(destination, args, &out, NULL))
		return TRUE;

	return *g_strstrip(out) != '\0';
}

/* ── Diff ────────────────────────────────────────────────────────── */

typedef struct {
	gchar *destination;
	gchar *base;
	gchar *head;
	gsize  max_bytes;
} DiffJob;

typedef struct {
	gchar    *diff;
	gboolean  truncated;
	gchar   **untracked;
} DiffResult;

static void
diff_job_free(
	DiffJob *job
){
	g_free(job->destination);
	g_free(job->base);
	g_free(job->head);
	g_free(job);
}

static void
diff_result_free(
	DiffResult *result
){
	g_free(result->diff);
	g_strfreev(result->untracked);
	g_free(result);
}

/*
 * In a worker thread: git's own commands, one after the other.  A diff
 * of a big change is big, and nothing on the main loop should wait for
 * it.
 */
static void
diff_in_thread(
	GTask        *task,
	gpointer      source,
	gpointer      task_data,
	GCancellable *cancellable
){
	DiffJob *job = task_data;
	DiffResult *result = g_new0(DiffResult, 1);
	const gchar *diff_args[] = { "diff", "--no-color", "--no-ext-diff",
	                             job->base, job->head, NULL };
	const gchar *new_args[] = { "ls-files", "--others", "--exclude-standard",
	                            NULL };
	g_autofree gchar *untracked = NULL;
	GError *error = NULL;

	(void)source;
	(void)cancellable;

	if (!run_git(job->destination, diff_args, &result->diff, &error)) {
		diff_result_free(result);
		g_task_return_error(task, error);
		return;
	}

	/* Cut on a line boundary, so the last line shown is whole. */
	if (strlen(result->diff) > job->max_bytes) {
		gchar *cut = g_strrstr_len(result->diff, (gssize)job->max_bytes,
		                           "\n");

		if (cut != NULL)
			cut[1] = '\0';
		else
			result->diff[job->max_bytes] = '\0';
		result->truncated = TRUE;
	}

	/* Untracked files exist only in a working checkout, not on a branch. */
	if (job->head == NULL &&
	    run_git(job->destination, new_args, &untracked, NULL)) {
		g_auto(GStrv) lines = g_strsplit(g_strstrip(untracked), "\n", -1);
		g_autoptr(GStrvBuilder) names = g_strv_builder_new();
		guint i;

		for (i = 0; lines[i] != NULL; i++) {
			if (*lines[i] != '\0')
				g_strv_builder_add(names, lines[i]);
		}

		result->untracked = g_strv_builder_end(names);
	}

	g_task_return_pointer(task, result, (GDestroyNotify)diff_result_free);
}

void
clawt_task_checkout_diff_async(
	const gchar         *destination,
	const gchar         *base,
	const gchar         *head,
	gsize                max_bytes,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	g_autoptr(GTask) task = NULL;
	DiffJob *job;

	g_return_if_fail(destination != NULL);
	g_return_if_fail(base != NULL);

	job = g_new0(DiffJob, 1);
	job->destination = g_strdup(destination);
	job->base = g_strdup(base);
	job->head = g_strdup(head);
	job->max_bytes = MAX(max_bytes, 1024);

	task = g_task_new(NULL, cancellable, callback, user_data);
	g_task_set_source_tag(task, clawt_task_checkout_diff_async);
	g_task_set_task_data(task, job, (GDestroyNotify)diff_job_free);
	g_task_run_in_thread(task, diff_in_thread);
}

gchar *
clawt_task_checkout_diff_finish(
	GAsyncResult   *result,
	gboolean       *out_truncated,
	gchar        ***out_untracked,
	GError        **error
){
	DiffResult *diff;
	gchar *text;

	g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);

	diff = g_task_propagate_pointer(G_TASK(result), error);

	if (diff == NULL)
		return NULL;

	if (out_truncated != NULL)
		*out_truncated = diff->truncated;
	if (out_untracked != NULL)
		*out_untracked = g_steal_pointer(&diff->untracked);

	text = g_steal_pointer(&diff->diff);
	diff_result_free(diff);

	return text;
}

/* ── For a task and its assignee ─────────────────────────────────── */

/*
 * A container connection is local when it is the user's socket: unset,
 * a socket path, or unix://.  Anything else (ssh://, tcp://, a named
 * connection) may be another machine, which cannot see this workspace.
 */
static gboolean
connection_is_local(
	const gchar *connection
){
	return connection == NULL || *connection == '\0' ||
	       *connection == '/' || g_str_has_prefix(connection, "unix:");
}

ClawtCheckoutMode
clawt_task_checkout_mode_for(
	ClawtAgentConfig  *config,
	GError           **error
){
	ClawtComputerType type;

	g_return_val_if_fail(config != NULL, CLAWT_CHECKOUT_NONE);

	type = (ClawtComputerType)clawt_agent_config_get_enum(config,
	                                                      "computer.type");

	if (type == CLAWT_COMPUTER_SSH) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
		            "%s works on an ssh host, which cannot see a checkout "
		            "made in its workspace here",
		            clawt_agent_config_get_id(config));
		return CLAWT_CHECKOUT_NONE;
	}

	if (type == CLAWT_COMPUTER_CONTAINER &&
	    !connection_is_local(clawt_agent_config_get_string(
	        config, "computer.container.connection"))) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
		            "%s works in a container on another podman "
		            "connection, which cannot see a checkout made here",
		            clawt_agent_config_get_id(config));
		return CLAWT_CHECKOUT_NONE;
	}

	/*
	 * A worktree's .git names a directory inside the repository: fine
	 * for tools on the host, unreachable from a sandbox or a machine
	 * that sees only the workspace.
	 */
	if (clawt_agent_confine_wanted(config) ||
	    type == CLAWT_COMPUTER_CONTAINER || type == CLAWT_COMPUTER_VM ||
	    type == CLAWT_COMPUTER_DISTROBOX)
		return CLAWT_CHECKOUT_CLONE;

	return CLAWT_CHECKOUT_WORKTREE;
}

/*
 * Where the checkout appears inside the agent's computer, when somewhere
 * else than on the host: the workspace mount of its own machine, or its
 * directory under the shared root on a shared one.
 */
static gchar *
guest_path_for(
	ClawtAgentConfig *config,
	const gchar      *short_id
){
	ClawtComputerType type = (ClawtComputerType)
		clawt_agent_config_get_enum(config, "computer.type");
	const gchar *shared = clawt_agent_config_get_string(config,
	                                                    "computer.use");

	if (type != CLAWT_COMPUTER_CONTAINER && type != CLAWT_COMPUTER_VM)
		return NULL;

	if (!clawt_agent_config_get_boolean(config, "computer.workspace"))
		return NULL;

	if (shared != NULL && *shared != '\0')
		return g_build_filename(CLAWT_SHARED_WORKSPACES_ROOT,
		                        clawt_agent_config_get_id(config), "tasks",
		                        short_id, NULL);

	return g_build_filename(CLAWT_WORKSPACE_MOUNT_POINT, "tasks", short_id,
	                        NULL);
}

gchar *
clawt_task_checkout_root_for(
	ClawtAgentConfig *config
){
	g_autofree gchar *workspace = NULL;

	g_return_val_if_fail(config != NULL, NULL);

	workspace = clawt_agent_config_get_workspace(config);

	return (workspace != NULL) ? g_build_filename(workspace, "tasks", NULL)
	                           : NULL;
}

gboolean
clawt_task_checkout_open(
	ClawtTask         *task,
	ClawtAgentConfig  *assignee,
	const gchar       *repository,
	const gchar       *base,
	GError           **error
){
	g_autofree gchar *expanded = NULL;
	g_autofree gchar *root = NULL;
	g_autofree gchar *branch = NULL;
	g_autofree gchar *destination = NULL;
	g_autofree gchar *commit = NULL;
	g_autofree gchar *guest = NULL;
	const gchar *short_id;
	ClawtCheckoutMode mode;

	g_return_val_if_fail(task != NULL, FALSE);
	g_return_val_if_fail(repository != NULL, FALSE);

	if (assignee == NULL) {
		g_set_error_literal(error, CLAWT_ERROR,
		                    CLAWT_ERROR_INVALID_ARGUMENT,
		                    "a checkout is made in its assignee's "
		                    "workspace, and this task has none yet");
		return FALSE;
	}

	expanded = clawt_expand_path(repository);

	if (!g_path_is_absolute(expanded) ||
	    !g_file_test(expanded, G_FILE_TEST_IS_DIR)) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
		            "%s is not a directory on this machine; name the "
		            "repository by its absolute host path", repository);
		return FALSE;
	}

	mode = clawt_task_checkout_mode_for(assignee, error);
	if (mode == CLAWT_CHECKOUT_NONE)
		return FALSE;

	root = clawt_task_checkout_root_for(assignee);
	if (root == NULL) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		            "%s has no workspace to put a checkout in",
		            clawt_agent_config_get_id(assignee));
		return FALSE;
	}

	short_id = clawt_task_get_id(task);
	if (g_str_has_prefix(short_id, "task-"))
		short_id += 5;

	branch = clawt_task_checkout_branch_for(clawt_task_get_id(task));
	destination = g_build_filename(root, short_id, NULL);

	if (!clawt_task_checkout_create(expanded, base, destination, branch,
	                                mode, &commit, error))
		return FALSE;

	guest = guest_path_for(assignee, short_id);

	clawt_task_set_checkout(task, expanded, destination, branch, commit,
	                        mode);
	clawt_task_set_checkout_guest(task, guest);

	return TRUE;
}

gchar *
clawt_task_checkout_guidance(
	ClawtTask *task
){
	g_autoptr(GString) out = NULL;
	const gchar *base;

	g_return_val_if_fail(task != NULL, NULL);

	if (clawt_task_get_checkout_mode(task) == CLAWT_CHECKOUT_NONE)
		return g_strdup("");

	base = clawt_task_get_checkout_base(task);
	out = g_string_new(NULL);

	g_string_append_printf(out,
		"\n\n[clawtilla] This task has a checkout of its own: %s",
		clawt_task_get_checkout_path(task));

	if (clawt_task_get_checkout_guest(task) != NULL)
		g_string_append_printf(out, " (inside your computer: %s)",
		                       clawt_task_get_checkout_guest(task));

	g_string_append_printf(out,
		", on branch %s, started from %.12s of %s. Work only there and "
		"commit on that branch; do not push, merge or touch %s itself "
		"unless you are asked to. What you changed is read from the "
		"checkout as a diff against where it started.",
		clawt_task_get_checkout_branch(task),
		base != NULL ? base : "",
		clawt_task_get_checkout_repo(task),
		clawt_task_get_checkout_repo(task));

	return g_string_free(g_steal_pointer(&out), FALSE);
}
