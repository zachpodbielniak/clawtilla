/*
 * clawt-project.c - Which project a piece of work belongs to
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "task/clawt-project.h"

/* The path as the file system names it: symlinks and `..` resolved. */
static gchar *
real_path(
	const gchar *path
){
	g_autoptr(GFile) file = NULL;
	g_autofree gchar *canonical = NULL;
	gchar resolved[PATH_MAX];

	if (path == NULL)
		return NULL;

	if (realpath(path, resolved) != NULL)
		return g_strdup(resolved);

	file = g_file_new_for_path(path);
	canonical = g_file_get_path(file);

	return g_steal_pointer(&canonical);
}

/* The canonical keys of @directory's remotes; empty when it has none. */
static GHashTable *
remote_keys(
	const gchar *directory
){
	GHashTable *keys = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                         NULL);
	g_autoptr(GPtrArray) remotes = NULL;
	guint i;

	if (directory == NULL || !g_file_test(directory, G_FILE_TEST_IS_DIR))
		return keys;

	remotes = clawt_git_remotes_for_dir(directory, NULL);

	for (i = 0; remotes != NULL && i < remotes->len; i++) {
		ClawtGitRemote *remote = g_ptr_array_index(remotes, i);

		if (remote->canonical != NULL)
			g_hash_table_add(keys, g_strdup(remote->canonical));
	}

	return keys;
}

static gboolean
shares_a_key(
	GHashTable *a,
	GHashTable *b
){
	GHashTableIter iter;
	gpointer key;

	g_hash_table_iter_init(&iter, a);

	while (g_hash_table_iter_next(&iter, &key, NULL)) {
		if (g_hash_table_contains(b, key))
			return TRUE;
	}

	return FALSE;
}

ClawtProjectSpec *
clawt_project_resolve(
	ClawtConfig  *config,
	const gchar  *named,
	const gchar  *parent_project,
	const gchar  *repository,
	GError      **error
){
	g_autoptr(GPtrArray) projects = NULL;
	g_autofree gchar *where = NULL;
	g_autoptr(GHashTable) keys = NULL;
	guint i;

	g_return_val_if_fail(CLAWT_IS_CONFIG(config), NULL);

	/* Named outright: that one, or an error saying it does not exist. */
	if (named != NULL && *named != '\0') {
		ClawtProjectSpec *spec = clawt_config_get_project(config, named);

		if (spec == NULL)
			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
			            "there is no project called '%s'", named);
		return spec;
	}

	/* A piece of a project's work is the project's work. */
	if (parent_project != NULL && *parent_project != '\0') {
		ClawtProjectSpec *spec = clawt_config_get_project(config,
		                                                  parent_project);

		if (spec != NULL)
			return spec;
	}

	if (repository == NULL || *repository == '\0')
		return NULL;

	projects = clawt_config_get_projects(config);

	if (projects->len == 0)
		return NULL;

	/*
	 * By path first -- free, and exact -- then by remotes, which costs a
	 * `git remote -v` per project directory and is only asked once the
	 * cheap answer has failed.
	 */
	where = real_path(repository);

	for (i = 0; i < projects->len; i++) {
		ClawtProjectSpec *spec = g_ptr_array_index(projects, i);
		g_autofree gchar *theirs = real_path(spec->directory);

		if (theirs != NULL && g_strcmp0(theirs, where) == 0)
			return g_ptr_array_steal_index(projects, i);
	}

	keys = remote_keys(where);

	if (g_hash_table_size(keys) == 0)
		return NULL;

	for (i = 0; i < projects->len; i++) {
		ClawtProjectSpec *spec = g_ptr_array_index(projects, i);
		g_autoptr(GHashTable) theirs = remote_keys(spec->directory);

		if (shares_a_key(keys, theirs))
			return g_ptr_array_steal_index(projects, i);
	}

	return NULL;
}

gint64
clawt_project_month_start(
	gint64 now
){
	g_autoptr(GDateTime) at = g_date_time_new_from_unix_local(now);
	g_autoptr(GDateTime) start = NULL;

	if (at == NULL)
		return 0;

	start = g_date_time_new_local(g_date_time_get_year(at),
	                              g_date_time_get_month(at), 1, 0, 0, 0);

	return (start != NULL) ? g_date_time_to_unix(start) : 0;
}
