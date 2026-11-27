/*
 * test-secret-ref.c - Command credential completion and deadlines
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <clawtilla.h>
#include "config/clawt-secret-ref-private.h"

#include <glib/gstdio.h>

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>

/* Mirrors CLAWT_SECRET_COMMAND_REAP_GRACE_SECONDS in clawt-secret-ref.c. */
#define TEST_REAP_GRACE_SECONDS (2)

/**
 * test_command_success:
 *
 * Exercise completion after the callback has returned, including CRLF trimming.
 */
static void
test_command_success(void)
{
	g_autoptr(ClawtSecretRef) ref = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *value = NULL;

	ref = clawt_secret_ref_new(CLAWT_SECRET_BACKEND_COMMAND,
		"printf 'demo\\r\\n'");
	value = clawt_secret_ref_resolve(ref, NULL, 2, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(value, ==, "demo");
}

/**
 * test_command_failure:
 * @data: command that must not produce a usable credential
 *
 * Nonzero exits and empty output must remain actionable errors.
 */
static void
test_command_failure(gconstpointer data)
{
	g_autoptr(ClawtSecretRef) ref = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *value = NULL;

	ref = clawt_secret_ref_new(CLAWT_SECRET_BACKEND_COMMAND,
		(const gchar *)data);
	value = clawt_secret_ref_resolve(ref, NULL, 2, &error);
	g_assert_null(value);
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_SECRET);
}

/**
 * test_command_timeout:
 * @data: shell command whose descendants retain stdout
 *
 * A dead direct child is insufficient: inherited pipes must not extend the
 * configured deadline, even when the parent exits before the timeout.
 */
static void
test_command_timeout(gconstpointer data)
{
	g_autoptr(ClawtSecretRef) ref = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *value = NULL;
	gint64 started;
	gint64 elapsed;

	ref = clawt_secret_ref_new(CLAWT_SECRET_BACKEND_COMMAND,
		(const gchar *)data);
	started = g_get_monotonic_time();
	value = clawt_secret_ref_resolve(ref, NULL, 1, &error);
	elapsed = g_get_monotonic_time() - started;
	g_assert_null(value);
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_TIMEOUT);
	g_assert_cmpint(elapsed, <, 3 * G_USEC_PER_SEC);
}

/**
 * snapshot_open_fds:
 *
 * Record every open descriptor in this isolated test process -- context
 * eventfds, child-watch pidfds and pipes included -- with what it refers to,
 * so a failure can name the descriptor rather than only count it.
 *
 * Returns: (transfer full): descriptor number (as a string) to its target
 */
static GHashTable *
snapshot_open_fds(void)
{
	g_autoptr(GDir) directory = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) names = NULL;
	GHashTable *fds;
	const gchar *name;
	guint i;

	/* Read the names first so the directory's own descriptor is gone. */
	names = g_ptr_array_new_with_free_func(g_free);
	directory = g_dir_open("/proc/self/fd", 0, &error);
	g_assert_no_error(error);
	while ((name = g_dir_read_name(directory)) != NULL)
		g_ptr_array_add(names, g_strdup(name));
	g_clear_pointer(&directory, g_dir_close);

	fds = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	for (i = 0; i < names->len; i++) {
		g_autofree gchar *path = NULL;
		gchar *target;

		path = g_build_filename("/proc/self/fd",
			(const gchar *)names->pdata[i], NULL);
		target = g_file_read_link(path, NULL);
		/* The listing's own descriptor is closed by now. */
		if (target == NULL)
			continue;
		g_hash_table_insert(fds, g_strdup((const gchar *)names->pdata[i]),
			target);
	}
	return fds;
}

/**
 * describe_new_fds:
 * @before: snapshot from snapshot_open_fds()
 * @after: later snapshot from snapshot_open_fds()
 *
 * A pidfd's fdinfo "Pid:" line is appended: -1 there means the process was
 * reaped and only the descriptor is left.
 *
 * Returns: (transfer full): "fd N -> target; " for each descriptor in @after
 *   that is not the same descriptor in @before, or an empty string
 */
static gchar *
describe_new_fds(GHashTable *before, GHashTable *after)
{
	GString *text;
	GHashTableIter iter;
	gpointer key;
	gpointer value;

	text = g_string_new(NULL);
	g_hash_table_iter_init(&iter, after);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		const gchar *old_target;
		g_autofree gchar *info_path = NULL;
		g_autofree gchar *info = NULL;

		old_target = g_hash_table_lookup(before, key);
		if (g_strcmp0(old_target, (const gchar *)value) == 0)
			continue;
		g_string_append_printf(text, "fd %s -> %s", (const gchar *)key,
			(const gchar *)value);
		info_path = g_strdup_printf("/proc/self/fdinfo/%s",
			(const gchar *)key);
		if (g_file_get_contents(info_path, &info, NULL, NULL)) {
			const gchar *pid_line = strstr(info, "\nPid:");

			if (pid_line != NULL)
				g_string_append_printf(text, " (%.*s)",
					(gint)strcspn(pid_line + 1, "\n"), pid_line + 1);
		}
		g_string_append(text, "; ");
	}
	return g_string_free(text, FALSE);
}

/**
 * test_command_timeout_cleanup:
 *
 * Every descriptor a resolution opens -- its private context's eventfd, the
 * child watch's pidfd, the stdout pipe -- is closed on the calling thread
 * before it returns, timed out or not, so after any number of resolutions
 * the process holds exactly the descriptors it held before them.  Nothing is
 * left for another thread to release later, so there is no allowance: a
 * single descriptor more fails, and the message names it.
 *
 * One timed-out and one successful resolution run first, so anything GLib
 * opens once per process on first use is in the baseline.
 */
static void
test_command_timeout_cleanup(void)
{
	g_autoptr(GHashTable) before = NULL;
	g_autoptr(GHashTable) after = NULL;
	g_autofree gchar *added = NULL;
	guint round;

	test_command_timeout("bash -c 'sleep 4; printf demo'");
	test_command_success();
	before = snapshot_open_fds();
	for (round = 0; round < 4; round++) {
		test_command_timeout("bash -c 'sleep 4; printf demo'");
		test_command_success();
	}
	after = snapshot_open_fds();
	added = describe_new_fds(before, after);
	g_assert_cmpstr(added, ==, "");
	g_assert_cmpuint(g_hash_table_size(after), ==, g_hash_table_size(before));
}

/**
 * child_state:
 * @pid: a child of this process
 *
 * Look without reaping: waitid() with WNOWAIT leaves a zombie in place.
 *
 * Returns: 0 while @pid is alive, 1 once it has exited but is unreaped,
 *   -1 once it is no longer this process's child at all (reaped)
 */
static gint
child_state(GPid pid)
{
	siginfo_t info;

	memset(&info, 0, sizeof(info));
	if (waitid(P_PID, (id_t)pid, &info, WEXITED | WNOHANG | WNOWAIT) != 0) {
		g_assert_cmpint(errno, ==, ECHILD);
		return -1;
	}
	return info.si_pid == pid ? 1 : 0;
}

/**
 * on_reap_deadline:
 * @user_data: (type gboolean *): set when the test's own deadline passes
 *
 * Returns: %G_SOURCE_REMOVE
 */
static gboolean
on_reap_deadline(gpointer user_data)
{
	*(gboolean *)user_data = TRUE;
	return G_SOURCE_REMOVE;
}

/**
 * test_command_unreaped_handoff:
 *
 * A timed-out command the kernel will not let go of -- uninterruptible
 * sleep, simulated by the private hook that withholds SIGKILL -- must not
 * hold the call past its deadline plus the reap grace.  The call returns
 * CLAWT_ERROR_TIMEOUT, warns with the pid and locator, and leaves the
 * pid with a child watch on the default context: still unreaped when the
 * call returns, reaped by that watch once it dies, and no zombie after.
 */
static void
test_command_unreaped_handoff(void)
{
	g_autoptr(ClawtSecretRef) ref = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GError) dir_error = NULL;
	g_autoptr(GSource) deadline = NULL;
	g_autofree gchar *directory = NULL;
	g_autofree gchar *pid_path = NULL;
	g_autofree gchar *locator = NULL;
	g_autofree gchar *pid_text = NULL;
	g_autofree gchar *value = NULL;
	gint64 started;
	gint64 elapsed;
	gboolean expired = FALSE;
	GPid pid;

	directory = g_dir_make_tmp("test-secret-ref-XXXXXX", &dir_error);
	g_assert_no_error(dir_error);
	pid_path = g_build_filename(directory, "pid", NULL);
	/* exec keeps the shell's pid, so the file names the direct child. */
	locator = g_strdup_printf("sh -c 'echo $$ > %s; exec sleep 60'",
		pid_path);
	ref = clawt_secret_ref_new(CLAWT_SECRET_BACKEND_COMMAND, locator);

	clawt_secret_ref_test_set_spare_on_timeout(TRUE);
	g_test_expect_message("Clawtilla", G_LOG_LEVEL_WARNING,
		"secret command (pid *) was not reaped * seconds after SIGKILL*");
	started = g_get_monotonic_time();
	value = clawt_secret_ref_resolve(ref, NULL, 1, &error);
	elapsed = g_get_monotonic_time() - started;
	g_test_assert_expected_messages();
	clawt_secret_ref_test_set_spare_on_timeout(FALSE);

	/* Deadline, then the whole grace, then returned: bounded both ways. */
	g_assert_null(value);
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_TIMEOUT);
	g_assert_cmpint(elapsed, >=, (1 + TEST_REAP_GRACE_SECONDS) *
		G_USEC_PER_SEC);
	g_assert_cmpint(elapsed, <, (1 + TEST_REAP_GRACE_SECONDS + 2) *
		G_USEC_PER_SEC);

	/* The command is alive, still our child, and nobody has reaped it. */
	g_assert_true(g_file_get_contents(pid_path, &pid_text, NULL, NULL));
	pid = (GPid)g_ascii_strtoll(pid_text, NULL, 10);
	g_assert_cmpint(pid, >, 0);
	g_assert_cmpint(child_state(pid), ==, 0);

	/*
	 * Let it go, then iterate the default context until its watch has
	 * reaped it.  The pidfd wakes the iteration; the test's own deadline
	 * only bounds a failure.
	 */
	g_assert_cmpint(kill(pid, SIGKILL), ==, 0);
	deadline = g_timeout_source_new_seconds(10);
	g_source_set_callback(deadline, on_reap_deadline, &expired, NULL);
	g_source_attach(deadline, NULL);
	while (child_state(pid) != -1 && !expired)
		g_main_context_iteration(NULL, TRUE);
	g_source_destroy(deadline);
	g_assert_false(expired);

	/* No zombie: the pid is no longer a child of this process at all. */
	g_assert_cmpint(waitpid(pid, NULL, WNOHANG), ==, -1);
	g_assert_cmpint(errno, ==, ECHILD);

	(void)g_remove(pid_path);
	(void)g_rmdir(directory);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/secret-ref/command/success", test_command_success);
	g_test_add_data_func("/secret-ref/command/nonzero",
		"bash -c 'printf demo; exit 1'", test_command_failure);
	g_test_add_data_func("/secret-ref/command/empty",
		"printf ''", test_command_failure);
	g_test_add_data_func("/secret-ref/command/newlines",
		"printf '\\r\\n'", test_command_failure);
	g_test_add_data_func("/secret-ref/command/invalid-utf8",
		"printf 'demo\\377'", test_command_failure);
	g_test_add_data_func("/secret-ref/command/timeout-child",
		"bash -c 'sleep 4; printf demo'", test_command_timeout);
	g_test_add_data_func("/secret-ref/command/timeout-exited-parent",
		"bash -c 'sleep 4 & exit 0'", test_command_timeout);
	/* setsid(1) forks when it leads a group, so sleep leaves the group. */
	g_test_add_data_func("/secret-ref/command/timeout-escaped-group",
		"setsid sleep 4", test_command_timeout);
	g_test_add_func("/secret-ref/command/timeout-cleanup",
		test_command_timeout_cleanup);
	g_test_add_func("/secret-ref/command/unreaped-handoff",
		test_command_unreaped_handoff);
	return g_test_run();
}
