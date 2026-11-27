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

	/*
	 * Deadline, then the whole grace, then returned: bounded both ways.  The
	 * lower bound is the behaviour -- the call waits out the grace.  The
	 * upper bound's extra 10 s is slack for a loaded machine (the suite runs
	 * beside a -j20 build), not part of the behaviour: it only catches a
	 * call that no longer stops waiting at all.
	 */
	g_assert_null(value);
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_TIMEOUT);
	g_assert_cmpint(elapsed, >=, (1 + TEST_REAP_GRACE_SECONDS) *
		G_USEC_PER_SEC);
	g_assert_cmpint(elapsed, <, (1 + TEST_REAP_GRACE_SECONDS + 10) *
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

/**
 * read_pid_file:
 * @path: a file a command wrote its pid to
 *
 * Wait (briefly, for a loaded machine) for @path to hold a pid, and read it.
 *
 * Returns: the pid, or 0 when none was written within 5 s
 */
static GPid
read_pid_file(const gchar *path)
{
	gint64 give_up;

	give_up = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
	do {
		g_autofree gchar *text = NULL;
		gint64 pid;

		if (g_file_get_contents(path, &text, NULL, NULL)) {
			pid = g_ascii_strtoll(text, NULL, 10);
			if (pid > 0)
				return (GPid)pid;
		}
		g_usleep(20 * 1000);
	} while (g_get_monotonic_time() < give_up);
	return 0;
}

/**
 * process_is_dead:
 * @pid: any process
 *
 * kill(pid, 0) succeeds on a zombie, and a killed grandchild is reaped by
 * whatever adopts it -- init, a subreaper, or nobody at all in a container
 * whose PID 1 never waits -- so "dead" is read from /proc: gone, or a
 * zombie (state Z) or dead (X) entry waiting for its new parent.
 *
 * Returns: %TRUE once @pid no longer runs
 */
static gboolean
process_is_dead(GPid pid)
{
	g_autofree gchar *path = NULL;
	g_autofree gchar *stat = NULL;
	const gchar *close_paren;

	path = g_strdup_printf("/proc/%" G_PID_FORMAT "/stat", pid);
	if (!g_file_get_contents(path, &stat, NULL, NULL))
		return TRUE;
	/* "pid (comm) S ...": comm may hold spaces and parentheses. */
	close_paren = strrchr(stat, ')');
	g_assert_nonnull(close_paren);
	return close_paren[1] == ' ' &&
		(close_paren[2] == 'Z' || close_paren[2] == 'X');
}

/**
 * wait_until_dead:
 * @pid: a process that should be dying
 *
 * Returns: %TRUE when @pid stopped running within the reap grace
 */
static gboolean
wait_until_dead(GPid pid)
{
	gint64 give_up;

	give_up = g_get_monotonic_time() +
		TEST_REAP_GRACE_SECONDS * G_USEC_PER_SEC;
	while (!process_is_dead(pid)) {
		if (g_get_monotonic_time() >= give_up)
			return FALSE;
		g_usleep(20 * 1000);
	}
	return TRUE;
}

/*
 * One run of test_command_kills_group(): the script either side of where
 * the pid file's path goes (split rather than a format string, so the
 * format stays a literal), and whether to run as on a kernel without
 * pidfd_open(2).
 */
typedef struct {
	const gchar *before_path;
	const gchar *after_path;
	gboolean     no_pidfd;
} KillsGroupCase;

/*
 * The leader is alive at the deadline; its background sleep never touches
 * stdout.  $! is that sleep: same group.
 */
static const KillsGroupCase kills_group_live = {
	"bash -c 'sleep 30 > /dev/null & echo $! > ", "; sleep 30'", FALSE
};
static const KillsGroupCase kills_group_live_no_pidfd = {
	"bash -c 'sleep 30 > /dev/null & echo $! > ", "; sleep 30'", TRUE
};

/*
 * The leader exits at once and its background sleep holds stdout, so the
 * deadline finds the leader already exited.  Only the group signal can
 * reach the sleep, and it may go only while the leader is unreaped -- so
 * this is a pidfd case: the fallback cannot do it, as command_kill() says.
 */
static const KillsGroupCase kills_group_after_exit = {
	"bash -c 'sleep 30 & echo $! > ", "; exit 0'", FALSE
};

/**
 * test_command_kills_group:
 * @data: (type KillsGroupCase): the script, and which wait to run it with
 *
 * A background descendant in the command's own process group dies with the
 * command at the deadline: the group signal reaches it, not only the direct
 * child -- including when the direct child exited first.
 */
static void
test_command_kills_group(gconstpointer data)
{
	const KillsGroupCase *run_case = data;
	g_autoptr(ClawtSecretRef) ref = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GError) dir_error = NULL;
	g_autofree gchar *directory = NULL;
	g_autofree gchar *pid_path = NULL;
	g_autofree gchar *locator = NULL;
	g_autofree gchar *value = NULL;
	GPid descendant;

	directory = g_dir_make_tmp("test-secret-ref-XXXXXX", &dir_error);
	g_assert_no_error(dir_error);
	pid_path = g_build_filename(directory, "pid", NULL);
	locator = g_strconcat(run_case->before_path, pid_path,
		run_case->after_path, NULL);
	ref = clawt_secret_ref_new(CLAWT_SECRET_BACKEND_COMMAND, locator);

	clawt_secret_ref_test_set_no_pidfd(run_case->no_pidfd);
	value = clawt_secret_ref_resolve(ref, NULL, 1, &error);
	clawt_secret_ref_test_set_no_pidfd(FALSE);
	g_assert_null(value);
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_TIMEOUT);

	descendant = read_pid_file(pid_path);
	g_assert_cmpint(descendant, >, 0);
	if (!wait_until_dead(descendant)) {
		(void)kill(descendant, SIGKILL);
		g_error("background descendant %" G_PID_FORMAT " outlived the "
		        "deadline: the group was not signalled", descendant);
	}

	(void)g_remove(pid_path);
	(void)g_rmdir(directory);
}

/**
 * test_command_no_group_kill_after_reap:
 * @data: (type gboolean): %TRUE to run as on a kernel without pidfd_open(2)
 *
 * The leader exits at once, and the one process that keeps stdout open
 * has moved to a session of its own, so the command's group is empty by
 * the deadline.  Once the leader is reaped that pgid number is free and
 * the kernel may hand it to a stranger, so no group signal may follow the
 * reap.  The seam records every signal and whether the leader had been
 * reaped when it went, which catches the hazard without racing pid reuse.
 *
 * With a pidfd the leader is left a zombie until after the kills, so the
 * group signal is still sent, safely; without one the group is not
 * signalled at all once the child watch has reaped the leader.
 *
 * The sleep in its own session is orphaned by design: no signal the
 * backend may send reaches it, and closing its stdout is what bounds the
 * call.  So only the deadline and the signals are asserted, and the test
 * kills the sleep itself.
 */
static void
test_command_no_group_kill_after_reap(gconstpointer data)
{
	g_autoptr(ClawtSecretRef) ref = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GError) dir_error = NULL;
	g_autoptr(GArray) records = NULL;
	g_autofree gchar *directory = NULL;
	g_autofree gchar *pid_path = NULL;
	g_autofree gchar *locator = NULL;
	g_autofree gchar *value = NULL;
	gboolean fallback;
	gboolean group_signalled;
	GPid escaped;
	guint i;

	fallback = GPOINTER_TO_INT(data);
	directory = g_dir_make_tmp("test-secret-ref-XXXXXX", &dir_error);
	g_assert_no_error(dir_error);
	pid_path = g_build_filename(directory, "pid", NULL);
	/*
	 * bash leads the group and exits.  Its background child is no group
	 * leader, so setsid(1) calls setsid() without forking; sh records its
	 * pid, which exec hands to the sleep that holds stdout.
	 */
	locator = g_strdup_printf("bash -c 'setsid sh -c \"echo \\$\\$ > %s; "
		"exec sleep 30\" & exit 0'", pid_path);
	ref = clawt_secret_ref_new(CLAWT_SECRET_BACKEND_COMMAND, locator);

	records = g_array_new(FALSE, TRUE, sizeof(ClawtSecretKillRecord));
	clawt_secret_ref_test_set_no_pidfd(fallback);
	clawt_secret_ref_test_record_kills(records);
	value = clawt_secret_ref_resolve(ref, NULL, 1, &error);
	clawt_secret_ref_test_record_kills(NULL);
	clawt_secret_ref_test_set_no_pidfd(FALSE);

	/* The escaped sleep is nobody's to kill but ours; never leave it. */
	escaped = read_pid_file(pid_path);
	if (escaped > 0)
		(void)kill(escaped, SIGKILL);
	(void)g_remove(pid_path);
	(void)g_rmdir(directory);

	g_assert_null(value);
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_TIMEOUT);
	g_assert_cmpint(escaped, >, 0);

	group_signalled = FALSE;
	for (i = 0; i < records->len; i++) {
		ClawtSecretKillRecord *record;

		record = &g_array_index(records, ClawtSecretKillRecord, i);
		if (record->target < 0) {
			group_signalled = TRUE;
			if (record->reaped)
				g_error("SIGKILL sent to group %d after its leader was "
				        "reaped: that pgid may belong to a stranger",
				        -record->target);
		}
		/* A pidfd keeps the leader unreaped through every kill. */
		if (!fallback)
			g_assert_false(record->reaped);
	}
	/* The zombie leader keeps the pgid ours, so the group is still hit. */
	if (!fallback)
		g_assert_true(group_signalled);
}

/**
 * count_open_pidfds:
 *
 * Returns: how many pidfds this process holds, by what /proc/self/fd says
 *   each descriptor is (anon_inode:[pidfd], or pidfd:[...] with pidfs)
 */
static guint
count_open_pidfds(void)
{
	g_autoptr(GHashTable) fds = NULL;
	GHashTableIter iter;
	gpointer value;
	guint count;

	fds = snapshot_open_fds();
	count = 0;
	g_hash_table_iter_init(&iter, fds);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		const gchar *target = value;

		if (strcmp(target, "anon_inode:[pidfd]") == 0 ||
		    g_str_has_prefix(target, "pidfd:"))
			count++;
	}
	return count;
}

/**
 * test_command_no_pidfd_left:
 *
 * Deterministic where the exact descriptor-set test is not: whatever else
 * GLib may hold, no pidfd -- neither the backend's own nor a child watch's
 * -- is open once a resolution returns, timed out or not.  A pidfd released
 * by another thread after the call (GSubprocess's worker) is still open at
 * this point, so this fails at once rather than now and then.
 */
static void
test_command_no_pidfd_left(void)
{
	g_assert_cmpuint(count_open_pidfds(), ==, 0);
	test_command_timeout("bash -c 'sleep 4 & exit 0'");
	g_assert_cmpuint(count_open_pidfds(), ==, 0);
	test_command_timeout("bash -c 'sleep 4; printf demo'");
	g_assert_cmpuint(count_open_pidfds(), ==, 0);
	test_command_success();
	g_assert_cmpuint(count_open_pidfds(), ==, 0);
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
	g_test_add_func("/secret-ref/command/no-pidfd-left",
		test_command_no_pidfd_left);
	g_test_add_data_func("/secret-ref/command/kills-group",
		&kills_group_live, test_command_kills_group);
	g_test_add_data_func("/secret-ref/command/kills-group-no-pidfd",
		&kills_group_live_no_pidfd, test_command_kills_group);
	g_test_add_data_func("/secret-ref/command/kills-group-after-exit",
		&kills_group_after_exit, test_command_kills_group);
	g_test_add_data_func("/secret-ref/command/no-group-kill-after-reap",
		GINT_TO_POINTER(FALSE), test_command_no_group_kill_after_reap);
	g_test_add_data_func(
		"/secret-ref/command/no-group-kill-after-reap-no-pidfd",
		GINT_TO_POINTER(TRUE), test_command_no_group_kill_after_reap);
	g_test_add_func("/secret-ref/command/unreaped-handoff",
		test_command_unreaped_handoff);
	return g_test_run();
}
