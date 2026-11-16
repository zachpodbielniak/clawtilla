/*
 * test-secret-ref.c - Command credential completion and deadlines
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <clawtilla.h>

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
 * count_open_fds:
 *
 * Count descriptors in this isolated test process, including context eventfds.
 *
 * Returns: number of open descriptors
 */
static guint
count_open_fds(void)
{
	g_autoptr(GDir) directory = NULL;
	g_autoptr(GError) error = NULL;
	guint count = 0;

	directory = g_dir_open("/proc/self/fd", 0, &error);
	g_assert_no_error(error);
	while (g_dir_read_name(directory) != NULL)
		count++;
	return count;
}

/**
 * test_command_timeout_cleanup:
 *
 * Warm up GIO's process-wide worker machinery, then verify that cancelled
 * operations release their private contexts instead of leaking an eventfd
 * for every resolution. Successful commands give exited-child reaping a
 * chance to settle without iterating any abandoned private context.
 */
static void
test_command_timeout_cleanup(void)
{
	guint baseline;
	guint round;

	test_command_timeout("bash -c 'sleep 4; printf demo'");
	test_command_success();
	baseline = count_open_fds();
	for (round = 0; round < 4; round++) {
		test_command_timeout("bash -c 'sleep 4; printf demo'");
		test_command_success();
	}
	/* Allow one descriptor for a process-reaper handoff still in flight. */
	g_assert_cmpuint(count_open_fds(), <=, baseline + 1);
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
	g_test_add_data_func("/secret-ref/command/timeout-child",
		"bash -c 'sleep 4; printf demo'", test_command_timeout);
	g_test_add_data_func("/secret-ref/command/timeout-exited-parent",
		"bash -c 'sleep 4 & exit 0'", test_command_timeout);
	g_test_add_func("/secret-ref/command/timeout-cleanup",
		test_command_timeout_cleanup);
	return g_test_run();
}
