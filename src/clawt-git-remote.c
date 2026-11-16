/*
 * clawt-git-remote.c - Which repository a directory is, by its remotes
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include <string.h>

#include "clawtilla.h"
#include "clawt-git-remote.h"

void
clawt_git_remote_free(
	ClawtGitRemote *remote
){
	if (remote == NULL)
		return;

	g_free(remote->name);
	g_free(remote->url);
	g_free(remote->canonical);
	g_free(remote);
}

/*
 * The tail every spelling may or may not carry: any number of trailing
 * slashes, then one `.git`, then slashes again (`repo.git/`).
 */
static void
strip_tail(
	GString *path
){
	while (path->len > 1 && path->str[path->len - 1] == '/')
		g_string_truncate(path, path->len - 1);

	if (path->len > 4 && g_str_has_suffix(path->str, ".git"))
		g_string_truncate(path, path->len - 4);

	while (path->len > 1 && path->str[path->len - 1] == '/')
		g_string_truncate(path, path->len - 1);
}

/* `host/path`, lower-cased, from a host and a path that may lead with `/`. */
static gchar *
network_key(
	const gchar *host,
	gsize        host_len,
	const gchar *path
){
	g_autoptr(GString) key = NULL;
	g_autofree gchar *lower = NULL;

	/* `~` paths (`git@h:~me/r`) and leading slashes say nothing. */
	while (*path == '/' || *path == '~')
		path++;

	if (host_len == 0 || *path == '\0')
		return NULL;

	key = g_string_new(NULL);
	g_string_append_len(key, host, (gssize)host_len);
	g_string_append_c(key, '/');
	g_string_append(key, path);
	strip_tail(key);

	/* Nothing after the host once the tail is gone: no repository. */
	if (key->len <= host_len + 1)
		return NULL;

	lower = g_ascii_strdown(key->str, (gssize)key->len);

	return g_steal_pointer(&lower);
}

gchar *
clawt_git_remote_canonical(
	const gchar *url
){
	g_autofree gchar *trimmed = NULL;
	const gchar *scheme_end;
	const gchar *colon;
	const gchar *slash;

	if (url == NULL)
		return NULL;

	trimmed = g_strstrip(g_strdup(url));

	if (*trimmed == '\0')
		return NULL;

	/*
	 * A local remote: `file://` or a bare path.  Its case is kept --
	 * a file system may care -- and only the tail is tidied.
	 */
	if (g_str_has_prefix(trimmed, "file://") || *trimmed == '/' ||
	    g_str_has_prefix(trimmed, "./") || g_str_has_prefix(trimmed, "../")) {
		g_autoptr(GString) path = g_string_new(
			g_str_has_prefix(trimmed, "file://") ? trimmed + 7 : trimmed);

		strip_tail(path);
		return g_string_free(g_steal_pointer(&path), FALSE);
	}

	scheme_end = strstr(trimmed, "://");

	/*
	 * `scheme://[user[:password]@]host[:port]/path`: the authority is
	 * everything up to the first slash after the scheme.
	 */
	if (scheme_end != NULL) {
		const gchar *authority = scheme_end + 3;
		const gchar *path = strchr(authority, '/');
		const gchar *host;
		const gchar *at;
		const gchar *end;

		if (path == NULL)
			return NULL;

		/* the last `@` in the authority ends the user part */
		host = authority;
		for (at = authority; at < path; at++) {
			if (*at == '@')
				host = at + 1;
		}

		/* `[v6]:port` keeps its brackets; otherwise a port follows `:` */
		if (*host == '[') {
			end = memchr(host, ']', (gsize)(path - host));
			end = (end != NULL) ? end + 1 : path;
		} else {
			end = memchr(host, ':', (gsize)(path - host));
			if (end == NULL)
				end = path;
		}

		return network_key(host, (gsize)(end - host), path);
	}

	/*
	 * scp-like: `[user@]host:path`.  git reads it so only when the colon
	 * comes before any slash; otherwise it is a relative path.
	 */
	colon = strchr(trimmed, ':');
	slash = strchr(trimmed, '/');

	if (colon != NULL && (slash == NULL || colon < slash)) {
		const gchar *host = trimmed;
		const gchar *at = memchr(trimmed, '@', (gsize)(colon - trimmed));

		if (at != NULL)
			host = at + 1;

		return network_key(host, (gsize)(colon - host), colon + 1);
	}

	/* A relative path git would resolve against the checkout. */
	{
		g_autoptr(GString) path = g_string_new(trimmed);

		strip_tail(path);
		return g_string_free(g_steal_pointer(&path), FALSE);
	}
}

GPtrArray *
clawt_git_remotes_parse(
	const gchar *output
){
	g_autoptr(GPtrArray) remotes = g_ptr_array_new_with_free_func(
		(GDestroyNotify)clawt_git_remote_free);
	g_auto(GStrv) lines = NULL;
	guint i;

	if (output == NULL)
		return g_steal_pointer(&remotes);

	lines = g_strsplit(output, "\n", -1);

	/*
	 * `origin\tgit@h:o/r.git (fetch)` -- one fetch and one push line
	 * per remote.  The fetch URL is the one that names where the work
	 * comes from; a remote with only a push line is skipped.
	 */
	for (i = 0; lines[i] != NULL; i++) {
		g_auto(GStrv) fields = NULL;
		g_autofree gchar *line = g_strstrip(g_strdup(lines[i]));
		ClawtGitRemote *remote;
		gboolean seen = FALSE;
		guint j;

		if (*line == '\0' || !g_str_has_suffix(line, "(fetch)"))
			continue;

		fields = g_regex_split_simple("\\s+", line, 0, 0);

		if (g_strv_length(fields) < 3)
			continue;

		for (j = 0; j < remotes->len; j++) {
			ClawtGitRemote *other = g_ptr_array_index(remotes, j);

			if (g_strcmp0(other->name, fields[0]) == 0)
				seen = TRUE;
		}

		if (seen)
			continue;

		remote = g_new0(ClawtGitRemote, 1);
		remote->name = g_strdup(fields[0]);
		remote->url = g_strdup(fields[1]);
		remote->canonical = clawt_git_remote_canonical(fields[1]);

		/* `origin` is tried first: it is where a checkout came from. */
		if (g_strcmp0(remote->name, "origin") == 0)
			g_ptr_array_insert(remotes, 0, remote);
		else
			g_ptr_array_add(remotes, remote);
	}

	return g_steal_pointer(&remotes);
}

/*
 * git, told where to look, with nothing of the user's config that could
 * prompt or page: no terminal prompt, no pager, and the directory marked
 * safe so a checkout owned by somebody else still answers.
 */
static GSubprocess *
spawn_remote_list(
	const gchar  *directory,
	GError      **error
){
	g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(
		G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_autofree gchar *safe = g_strdup_printf("safe.directory=%s", directory);

	g_subprocess_launcher_setenv(launcher, "GIT_TERMINAL_PROMPT", "0", TRUE);
	g_subprocess_launcher_setenv(launcher, "GIT_PAGER", "cat", TRUE);

	return g_subprocess_launcher_spawn(launcher, error, "git", "-c", safe,
	                                   "-C", directory, "remote", "-v",
	                                   NULL);
}

static GPtrArray *
remotes_from(
	GSubprocess  *process,
	const gchar  *directory,
	const gchar  *out,
	const gchar  *err,
	GError      **error
){
	if (!g_subprocess_get_successful(process)) {
		g_autofree gchar *said = g_strstrip(g_strdup(err != NULL ? err : ""));

		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		            "%s is not a git checkout%s%s", directory,
		            *said != '\0' ? ": " : "", said);
		return NULL;
	}

	return clawt_git_remotes_parse(out);
}

GPtrArray *
clawt_git_remotes_for_dir(
	const gchar  *directory,
	GError      **error
){
	g_autoptr(GSubprocess) process = NULL;
	g_autofree gchar *out = NULL;
	g_autofree gchar *err = NULL;

	g_return_val_if_fail(directory != NULL, NULL);

	process = spawn_remote_list(directory, error);

	if (process == NULL)
		return NULL;

	if (!g_subprocess_communicate_utf8(process, NULL, NULL, &out, &err,
	                                   error))
		return NULL;

	return remotes_from(process, directory, out, err, error);
}

static void
on_listed(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	g_autoptr(GTask) task = user_data;
	GSubprocess *process = G_SUBPROCESS(source);
	g_autofree gchar *out = NULL;
	g_autofree gchar *err = NULL;
	GError *error = NULL;
	GPtrArray *remotes;

	if (!g_subprocess_communicate_utf8_finish(process, result, &out, &err,
	                                          &error)) {
		g_task_return_error(task, error);
		return;
	}

	remotes = remotes_from(process, g_task_get_task_data(task), out, err,
	                       &error);

	if (remotes == NULL)
		g_task_return_error(task, error);
	else
		g_task_return_pointer(task, remotes,
		                      (GDestroyNotify)g_ptr_array_unref);
}

void
clawt_git_remotes_for_dir_async(
	const gchar         *directory,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	GTask *task;
	g_autoptr(GSubprocess) process = NULL;
	GError *error = NULL;

	g_return_if_fail(directory != NULL);

	task = g_task_new(NULL, cancellable, callback, user_data);
	g_task_set_source_tag(task, clawt_git_remotes_for_dir_async);
	g_task_set_task_data(task, g_strdup(directory), g_free);

	process = spawn_remote_list(directory, &error);

	if (process == NULL) {
		g_task_return_error(task, error);
		g_object_unref(task);
		return;
	}

	g_subprocess_communicate_utf8_async(process, NULL, cancellable,
	                                    on_listed, task);
}

GPtrArray *
clawt_git_remotes_for_dir_finish(
	GAsyncResult  *result,
	GError       **error
){
	g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);

	return g_task_propagate_pointer(G_TASK(result), error);
}
