/*
 * test-task-checkout.c - Task checkouts and the remote URL helper
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * - clawt_git_remote_canonical(): every spelling of a repository git
 *   accepts comes to the same key, and different repositories do not;
 * - clawt_git_remotes_parse(): fetch remotes once each, origin first;
 * - the checkout shape chosen per agent: worktree on the host, clone in a
 *   sandbox or a machine, none on ssh or a remote podman connection;
 * - a worktree and a clone made on a scratch repository, their diff
 *   (committed, uncommitted, untracked, cut short), dirtiness, removal
 *   keeping the branch -- a clone's fetched back -- and removal refused
 *   outside its root;
 * - the checkout paragraph of the assignment, and the fields surviving a
 *   round trip through JSON.
 *
 * Tests that need git skip themselves when it is not installed.
 */

#include "clawtilla.h"
#include "clawt-test-util.h"

#include <glib/gstdio.h>
#include <string.h>

/* ── The canonical key ───────────────────────────────────────────── */

static void
test_canonical_table(void)
{
	static const struct {
		const gchar *url;
		const gchar *key;
	} cases[] = {
		{ "git@gitlab.com:me/proj.git", "gitlab.com/me/proj" },
		{ "https://gitlab.com/me/proj", "gitlab.com/me/proj" },
		{ "https://gitlab.com/me/proj.git/", "gitlab.com/me/proj" },
		{ "ssh://git@gitlab.com:22/me/proj.git", "gitlab.com/me/proj" },
		{ "ssh://git@GitLab.COM/Me/Proj.git", "gitlab.com/me/proj" },
		{ "http://user:secret@gitlab.com:8080/me/proj", "gitlab.com/me/proj" },
		{ "git://gitlab.com/me/proj.git", "gitlab.com/me/proj" },
		{ "ssh://git@git-ssh.example.org:2222/me/proj.git",
		  "git-ssh.example.org/me/proj" },
		{ "git@git-ssh.example.org:~me/proj.git",
		  "git-ssh.example.org/me/proj" },
		{ "https://gitlab.com/group/sub/proj.git",
		  "gitlab.com/group/sub/proj" },
		{ "file:///srv/git/Proj.git", "/srv/git/Proj" },
		{ "/srv/git/Proj.git/", "/srv/git/Proj" },
		{ "../sibling.git", "../sibling" },
		{ "  https://gitlab.com/me/proj  ", "gitlab.com/me/proj" },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autofree gchar *key = clawt_git_remote_canonical(cases[i].url);

		g_test_message("%s", cases[i].url);
		g_assert_cmpstr(key, ==, cases[i].key);
	}

	/* nothing that names a repository */
	g_assert_null(clawt_git_remote_canonical(NULL));
	g_assert_null(clawt_git_remote_canonical(""));
	g_assert_null(clawt_git_remote_canonical("https://gitlab.com"));
	g_assert_null(clawt_git_remote_canonical("https://gitlab.com/"));
	g_assert_null(clawt_git_remote_canonical("git@gitlab.com:"));

	/* two repositories stay two */
	{
		g_autofree gchar *a = clawt_git_remote_canonical(
			"git@gitlab.com:me/proj.git");
		g_autofree gchar *b = clawt_git_remote_canonical(
			"git@gitlab.com:me/proj-two.git");

		g_assert_cmpstr(a, !=, b);
	}
}

static void
test_remotes_parse(void)
{
	g_autoptr(GPtrArray) remotes = clawt_git_remotes_parse(
		"mirror\tgit@git-ssh.example.org:me/proj.git (fetch)\n"
		"mirror\tgit@git-ssh.example.org:me/proj.git (push)\n"
		"origin\thttps://gitlab.com/me/proj.git (fetch)\n"
		"origin\thttps://gitlab.com/me/proj.git (push)\n"
		"pushonly\tgit@x:y/z (push)\n"
		"\n");
	ClawtGitRemote *first;
	ClawtGitRemote *second;

	g_assert_cmpuint(remotes->len, ==, 2);
	first = g_ptr_array_index(remotes, 0);
	second = g_ptr_array_index(remotes, 1);
	g_assert_cmpstr(first->name, ==, "origin");
	g_assert_cmpstr(first->canonical, ==, "gitlab.com/me/proj");
	g_assert_cmpstr(second->name, ==, "mirror");
	g_assert_cmpstr(second->canonical, ==, "git-ssh.example.org/me/proj");

	{
		g_autoptr(GPtrArray) none = clawt_git_remotes_parse(NULL);

		g_assert_cmpuint(none->len, ==, 0);
	}
}

/* ── Which shape, for which agent ────────────────────────────────── */

static ClawtCheckoutMode
mode_for_yaml(
	const gchar  *agent_yaml,
	GError      **error
){
	g_autofree gchar *yaml = g_strdup_printf("agents:\n%s", agent_yaml);
	g_autoptr(ClawtConfig) config = clawt_config_load_from_string(yaml,
	                                                              NULL);

	g_assert_nonnull(config);
	return clawt_task_checkout_mode_for(
		clawt_config_get_agent(config, "a"), error);
}

static void
test_mode_for_agent(void)
{
	g_autoptr(GError) error = NULL;

	g_assert_cmpint(mode_for_yaml("  - id: a\n", NULL), ==,
	                CLAWT_CHECKOUT_WORKTREE);
	g_assert_cmpint(mode_for_yaml(
		"  - id: a\n    computer:\n      type: host\n", NULL), ==,
		CLAWT_CHECKOUT_WORKTREE);
	g_assert_cmpint(mode_for_yaml(
		"  - id: a\n    runtime:\n      confine: bwrap\n", NULL), ==,
		CLAWT_CHECKOUT_CLONE);
	g_assert_cmpint(mode_for_yaml(
		"  - id: a\n    computer:\n      type: container\n", NULL), ==,
		CLAWT_CHECKOUT_CLONE);
	g_assert_cmpint(mode_for_yaml(
		"  - id: a\n    computer:\n      type: container\n"
		"      container:\n        connection: /run/user/1000/podman.sock\n",
		NULL), ==, CLAWT_CHECKOUT_CLONE);

	g_assert_cmpint(mode_for_yaml(
		"  - id: a\n    computer:\n      type: container\n"
		"      container:\n        connection: ssh://me@far/run/podman.sock\n",
		&error), ==, CLAWT_CHECKOUT_NONE);
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED);
	g_clear_error(&error);

	g_assert_cmpint(mode_for_yaml(
		"  - id: a\n    computer:\n      type: ssh\n"
		"      ssh:\n        host: far\n", &error), ==,
		CLAWT_CHECKOUT_NONE);
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED);
}

/* ── On a scratch repository ─────────────────────────────────────── */

static gboolean
have_git(void)
{
	g_autofree gchar *git = g_find_program_in_path("git");

	if (git == NULL) {
		g_test_skip("git is not installed");
		return FALSE;
	}

	return TRUE;
}

/* `git ...` in @dir, which must succeed; nothing of the user's config. */
static void
git_in(
	const gchar *dir,
	const gchar *args
){
	g_autofree gchar *command = g_strdup_printf(
		"env GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1 "
		"git -C '%s' -c user.name=t -c user.email=t@example.org "
		"-c init.defaultBranch=main -c commit.gpgsign=false %s",
		dir, args);
	g_autofree gchar *err = NULL;
	gint status = 0;

	g_assert_true(g_spawn_command_line_sync(command, NULL, &err, &status,
	                                        NULL));
	if (status != 0)
		g_error("%s: %s", command, err);
}

static gchar *
scratch_repository(
	const gchar *root
){
	gchar *repo = g_build_filename(root, "repo", NULL);
	g_autofree gchar *file = g_build_filename(repo, "README", NULL);

	g_assert_cmpint(g_mkdir_with_parents(repo, 0700), ==, 0);
	git_in(repo, "init -q");
	g_assert_true(g_file_set_contents(file, "one\n", -1, NULL));
	git_in(repo, "add README");
	git_in(repo, "commit -q -m first");

	return repo;
}

typedef struct {
	GMainLoop *loop;
	gchar     *diff;
	gboolean   truncated;
	gchar    **untracked;
} DiffWait;

static void
on_diff(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	DiffWait *wait = user_data;
	g_autoptr(GError) error = NULL;

	(void)source;

	wait->diff = clawt_task_checkout_diff_finish(result, &wait->truncated,
	                                             &wait->untracked, &error);
	g_assert_no_error(error);
	g_main_loop_quit(wait->loop);
}

static void
diff_of(
	const gchar *where,
	const gchar *base,
	const gchar *head,
	gsize        max_bytes,
	DiffWait    *wait
){
	wait->loop = g_main_loop_new(NULL, FALSE);
	clawt_task_checkout_diff_async(where, base, head, max_bytes, NULL,
	                               on_diff, wait);
	g_main_loop_run(wait->loop);
	g_main_loop_unref(wait->loop);
}

static void
diff_wait_clear(
	DiffWait *wait
){
	g_clear_pointer(&wait->diff, g_free);
	g_clear_pointer(&wait->untracked, g_strfreev);
}

/* Both shapes: made, changed, read as a diff, removed with the branch kept. */
static void
check_shape(
	ClawtCheckoutMode mode
){
	g_autofree gchar *root = g_dir_make_tmp("clawt-checkout-XXXXXX", NULL);
	g_autofree gchar *repo = scratch_repository(root);
	g_autofree gchar *tasks = g_build_filename(root, "ws", "tasks", NULL);
	g_autofree gchar *dest = g_build_filename(tasks, "01abc", NULL);
	g_autofree gchar *base = NULL;
	g_autofree gchar *readme = g_build_filename(dest, "README", NULL);
	g_autofree gchar *added = g_build_filename(dest, "NEW", NULL);
	g_autoptr(GError) error = NULL;
	DiffWait wait = { 0 };

	g_assert_true(clawt_task_checkout_create(repo, NULL, dest,
	                                         "clawtilla/01abc", mode, &base,
	                                         &error));
	g_assert_no_error(error);
	g_assert_cmpuint(strlen(base), ==, 40);
	g_assert_true(g_file_test(readme, G_FILE_TEST_EXISTS));
	g_assert_false(clawt_task_checkout_is_dirty(dest));

	/* made only fresh */
	g_assert_false(clawt_task_checkout_create(repo, NULL, dest, "other",
	                                          mode, NULL, &error));
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	/* a commit, then an uncommitted change and a new file: one diff */
	g_assert_true(g_file_set_contents(readme, "one\ntwo\n", -1, NULL));
	git_in(dest, "commit -q -am second");
	g_assert_true(g_file_set_contents(readme, "one\ntwo\nthree\n", -1,
	                                  NULL));
	g_assert_true(g_file_set_contents(added, "new\n", -1, NULL));
	g_assert_true(clawt_task_checkout_is_dirty(dest));

	diff_of(dest, base, NULL, 1 << 20, &wait);
	g_assert_nonnull(strstr(wait.diff, "+two"));
	g_assert_nonnull(strstr(wait.diff, "+three"));
	g_assert_false(wait.truncated);
	g_assert_cmpuint(g_strv_length(wait.untracked), ==, 1);
	g_assert_cmpstr(wait.untracked[0], ==, "NEW");
	diff_wait_clear(&wait);

	/* cut short, on a whole line */
	diff_of(dest, base, NULL, 1024, &wait);
	diff_wait_clear(&wait);
	{
		g_autoptr(GString) big = g_string_new(NULL);
		guint i;

		for (i = 0; i < 400; i++)
			g_string_append_printf(big, "line %u of a long change\n", i);
		g_assert_true(g_file_set_contents(readme, big->str, -1, NULL));
	}
	diff_of(dest, base, NULL, 1024, &wait);
	g_assert_true(wait.truncated);
	g_assert_cmpuint(strlen(wait.diff), <=, 1024);
	g_assert_true(g_str_has_suffix(wait.diff, "\n"));
	diff_wait_clear(&wait);

	/* removal is refused outside its root */
	g_assert_false(clawt_task_checkout_remove(repo, repo, dest,
	                                          "clawtilla/01abc", mode,
	                                          &error));
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);
	g_assert_true(g_file_test(dest, G_FILE_TEST_IS_DIR));

	/* removed: gone, and the committed work is on the branch in repo */
	git_in(dest, "checkout -q -- README");
	g_assert_true(clawt_task_checkout_remove(repo, tasks, dest,
	                                         "clawtilla/01abc", mode,
	                                         &error));
	g_assert_no_error(error);
	g_assert_false(g_file_test(dest, G_FILE_TEST_EXISTS));

	diff_of(repo, base, "clawtilla/01abc", 1 << 20, &wait);
	g_assert_nonnull(strstr(wait.diff, "+two"));
	g_assert_null(wait.untracked != NULL && wait.untracked[0] != NULL
	              ? wait.untracked[0] : NULL);
	diff_wait_clear(&wait);

	/* removing what is already gone is success */
	g_assert_true(clawt_task_checkout_remove(repo, tasks, dest,
	                                         "clawtilla/01abc", mode,
	                                         NULL));

	clawt_remove_tree(root, g_get_tmp_dir(), NULL);
}

static void
test_worktree(void)
{
	if (have_git())
		check_shape(CLAWT_CHECKOUT_WORKTREE);
}

static void
test_clone(void)
{
	if (have_git())
		check_shape(CLAWT_CHECKOUT_CLONE);
}

/* A base ref: the checkout starts there, and a bad one refuses. */
static void
test_base_ref(void)
{
	g_autofree gchar *root = NULL;
	g_autofree gchar *repo = NULL;
	g_autofree gchar *dest = NULL;
	g_autofree gchar *first = NULL;
	g_autofree gchar *base = NULL;
	g_autofree gchar *readme = NULL;
	g_autoptr(GError) error = NULL;

	if (!have_git())
		return;

	root = g_dir_make_tmp("clawt-checkout-XXXXXX", NULL);
	repo = scratch_repository(root);
	readme = g_build_filename(repo, "README", NULL);
	git_in(repo, "tag v1");
	g_assert_true(g_file_set_contents(readme, "later\n", -1, NULL));
	git_in(repo, "commit -q -am later");

	dest = g_build_filename(root, "ws", "tasks", "x", NULL);
	g_assert_true(clawt_task_checkout_create(repo, "v1", dest,
	                                         "clawtilla/x",
	                                         CLAWT_CHECKOUT_WORKTREE, &base,
	                                         &error));
	g_assert_no_error(error);
	{
		g_autofree gchar *path = g_build_filename(dest, "README", NULL);

		g_assert_true(g_file_get_contents(path, &first, NULL, NULL));
		g_assert_cmpstr(first, ==, "one\n");
	}

	g_clear_pointer(&dest, g_free);
	dest = g_build_filename(root, "ws", "tasks", "y", NULL);
	g_assert_false(clawt_task_checkout_create(repo, "no-such-ref", dest,
	                                          "clawtilla/y",
	                                          CLAWT_CHECKOUT_WORKTREE, NULL,
	                                          &error));
	g_assert_nonnull(error);
	g_assert_false(g_file_test(dest, G_FILE_TEST_EXISTS));

	clawt_remove_tree(root, g_get_tmp_dir(), NULL);
}

/* ── On the task ─────────────────────────────────────────────────── */

static void
test_task_fields_and_guidance(void)
{
	g_autoptr(ClawtTask) task = clawt_task_new("chief", "builder", "fix it");
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(ClawtTask) back = NULL;
	g_autofree gchar *none = NULL;
	g_autofree gchar *said = NULL;
	g_autofree gchar *whole = NULL;
	g_autofree gchar *branch = clawt_task_checkout_branch_for("task-01abc");

	g_assert_cmpstr(branch, ==, "clawtilla/01abc");

	none = clawt_task_checkout_guidance(task);
	g_assert_cmpstr(none, ==, "");

	clawt_task_set_checkout(task, "/src/proj", "/ws/tasks/01abc",
	                        "clawtilla/01abc",
	                        "0123456789abcdef0123456789abcdef01234567",
	                        CLAWT_CHECKOUT_CLONE);
	clawt_task_set_checkout_guest(task,
	                              "/mnt/clawtilla/workspace/tasks/01abc");

	said = clawt_task_checkout_guidance(task);
	g_assert_true(g_str_has_prefix(said, "\n\n[clawtilla] "));
	g_assert_nonnull(strstr(said, "/ws/tasks/01abc"));
	g_assert_nonnull(strstr(said, "/mnt/clawtilla/workspace/tasks/01abc"));
	g_assert_nonnull(strstr(said, "clawtilla/01abc"));
	g_assert_nonnull(strstr(said, "0123456789ab of /src/proj"));
	g_assert_null(strstr(said, "0123456789abc "));

	/* the contract still ends the assignment, as the daemon matches it */
	whole = clawt_task_assignment_guidance_for(task);
	{
		g_autofree gchar *contract = clawt_task_assignment_guidance(
			clawt_task_get_id(task));

		g_assert_true(g_str_has_prefix(whole, said));
		g_assert_true(g_str_has_suffix(whole, contract));
	}

	node = clawt_task_to_json(task);
	back = clawt_task_new_from_json(node, NULL);
	g_assert_cmpint(clawt_task_get_checkout_mode(back), ==,
	                CLAWT_CHECKOUT_CLONE);
	g_assert_cmpstr(clawt_task_get_checkout_repo(back), ==, "/src/proj");
	g_assert_cmpstr(clawt_task_get_checkout_path(back), ==,
	                "/ws/tasks/01abc");
	g_assert_cmpstr(clawt_task_get_checkout_branch(back), ==,
	                "clawtilla/01abc");
	g_assert_cmpstr(clawt_task_get_checkout_guest(back), ==,
	                "/mnt/clawtilla/workspace/tasks/01abc");

	/* cleared */
	clawt_task_set_checkout(task, NULL, NULL, NULL, NULL,
	                        CLAWT_CHECKOUT_WORKTREE);
	g_assert_cmpint(clawt_task_get_checkout_mode(task), ==,
	                CLAWT_CHECKOUT_NONE);
}

int
main(
	int    argc,
	char **argv
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/git-remote/canonical", test_canonical_table);
	g_test_add_func("/git-remote/parse", test_remotes_parse);
	g_test_add_func("/task-checkout/mode-for-agent", test_mode_for_agent);
	g_test_add_func("/task-checkout/worktree", test_worktree);
	g_test_add_func("/task-checkout/clone", test_clone);
	g_test_add_func("/task-checkout/base-ref", test_base_ref);
	g_test_add_func("/task-checkout/task", test_task_fields_and_guidance);

	return g_test_run();
}
