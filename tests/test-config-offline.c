/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Offline candidate validation must neither need a daemon nor write state. */
#include <clawtilla.h>
#include <glib/gstdio.h>
#include <string.h>
#include "clawt-test-util.h"

/* Exercise actual files, including the missing-file behavior that differs
 * intentionally from normal first-run configuration loading. */
static void
test_candidate_files(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("clawt-offline-XXXXXX", NULL);
	g_autofree gchar *path = g_build_filename(dir, "candidate.yaml", NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) warnings = NULL;
	g_autofree gchar *after = NULL;
	const gchar *valid = "agents: []\n";

	g_assert_false(clawt_config_validate_file(path, FALSE, &warnings, &error));
	g_assert_nonnull(error);
	g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
	g_clear_error(&error);
	g_clear_pointer(&warnings, g_ptr_array_unref);
	g_assert_true(g_file_set_contents(path, valid, -1, &error));
	g_assert_true(clawt_config_validate_file(path, TRUE, &warnings, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(warnings->len, ==, 0);
	g_assert_true(g_file_get_contents(path, &after, NULL, &error));
	g_assert_cmpstr(after, ==, valid);
	g_clear_pointer(&warnings, g_ptr_array_unref);

	g_assert_true(g_file_set_contents(path,
		"orchestration:\n  max_hops: 0\nunknown_key: true\n", -1, &error));
	g_assert_false(clawt_config_validate_file(path, FALSE, &warnings, &error));
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID);
	g_assert_cmpuint(warnings->len, >, 0);
	g_assert_nonnull(strstr(error->message, "orchestration.max_hops"));
	g_clear_error(&error);
	g_clear_pointer(&warnings, g_ptr_array_unref);

	g_assert_true(g_file_set_contents(path, "agents: [\n", -1, &error));
	g_assert_false(clawt_config_validate_file(path, FALSE, NULL, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_true(g_file_set_contents(path, "- not-a-mapping\n", -1, &error));
	g_assert_false(clawt_config_validate_file(path, FALSE, NULL, &error));
	g_assert_nonnull(error);
	clawt_test_remove_tree(dir);
}

/* Warning policy applies to unknown keys and shadow/ignored entries using
 * exactly the diagnostics that daemon loading already produces. */
static void
test_warning_policy(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("clawt-offline-XXXXXX", NULL);
	g_autofree gchar *path = g_build_filename(dir, "candidate.yaml", NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) warnings = NULL;

	g_assert_true(g_file_set_contents(path,
		"unknown_key: true\nagents:\n  - id: user\n  - name: missing-id\n",
		-1, &error));
	g_assert_true(clawt_config_validate_file(path, FALSE, &warnings, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(warnings->len, >=, 3);
	g_clear_pointer(&warnings, g_ptr_array_unref);
	g_assert_false(clawt_config_validate_file(path, TRUE, &warnings, &error));
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID);
	g_assert_nonnull(strstr(error->message, "strict validation"));
	g_assert_cmpuint(warnings->len, >=, 3);
	clawt_test_remove_tree(dir);
}

typedef struct {
	GMainLoop *loop;
	GSubprocess *process;
	gchar *out;
	gchar *err;
	GError *error;
	gboolean timed_out;
} CliRun;

/* A routing regression must fail rather than wait indefinitely on IPC. */
static gboolean
cli_timeout(gpointer data)
{
	CliRun *run = (CliRun *)data;

	run->timed_out = TRUE;
	g_subprocess_force_exit(run->process);
	return G_SOURCE_REMOVE;
}

/* Communication completion also reaps the subprocess after a timeout. */
static void
cli_finished(GObject *source, GAsyncResult *result, gpointer data)
{
	CliRun *run = (CliRun *)data;

	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result,
		&run->out, &run->err, &run->error);
	g_main_loop_quit(run->loop);
}

/* Invoke the real parser with an isolated home, nonexistent socket and a
 * candidate path containing spaces. No daemon or infrastructure is needed. */
static void
test_cli(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("clawt-offline-XXXXXX", NULL);
	g_autofree gchar *path = g_build_filename(dir, "candidate file.yaml", NULL);
	g_autofree gchar *socket = g_build_filename(dir, "absent.sock", NULL);
	g_autofree gchar *home_state = g_build_filename(dir, ".clawtilla", NULL);
	g_autofree gchar *state = g_build_filename(dir, "state", NULL);
	g_autofree gchar *workspace = g_build_filename(dir, "agents", NULL);
	g_autofree gchar *config = g_strdup_printf(
		"daemon:\n  state_dir: '%s'\ndefaults:\n  workspace_root: '%s'\n"
		"unknown_key: true\nagents: []\n", state, workspace);
	g_autofree gchar *binary = g_build_filename(CLAWT_TEST_SRCDIR,
		BUILD_OUTDIR, "clawtilla", NULL);
	g_autofree gchar *after = NULL;
	guint i;
	const gchar *tails[][6] = {
		{ "--file", path, NULL },
		{ "--file", path, "--strict", NULL },
		{ "--file", socket, NULL },
		{ "--file", path, "--typo", NULL },
		{ "--strict", NULL },
		{ "--file", path, "extra", NULL },
		{ "-f", path, NULL },
		{ "--help", NULL },
		{ "--license", NULL }
	};

	g_assert_true(g_file_set_contents(path, config, -1, NULL));
	for (i = 0; i < G_N_ELEMENTS(tails); i++) {
		g_autoptr(GSubprocessLauncher) launcher = NULL;
		g_autoptr(GSubprocess) process = NULL;
		g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
		const gchar *args[16] = { binary, "-s", socket, "-c", socket,
			"config", "validate", NULL };
		CliRun run = { 0 };
		guint j;
		guint timeout;

		for (j = 0; tails[i][j] != NULL; j++)
			args[7 + j] = tails[i][j];
		launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
			G_SUBPROCESS_FLAGS_STDERR_PIPE);
		g_subprocess_launcher_setenv(launcher, "HOME", dir, TRUE);
		process = g_subprocess_launcher_spawnv(launcher, args, &run.error);
		g_assert_no_error(run.error);
		g_assert_nonnull(process);
		run.loop = loop;
		run.process = process;
		timeout = g_timeout_add_seconds(10, cli_timeout, &run);
		g_subprocess_communicate_utf8_async(process, NULL, NULL,
			cli_finished, &run);
		g_main_loop_run(loop);
		if (!run.timed_out)
			g_source_remove(timeout);
		g_assert_false(run.timed_out);
		g_assert_no_error(run.error);
		g_assert_true(g_subprocess_get_if_exited(process));
		g_assert_cmpint(g_subprocess_get_exit_status(process), ==,
			(i == 0 || i >= 6) ? 0 : 1);
		if (i == 0 || i == 1)
			g_assert_nonnull(strstr(run.err, "unknown_key"));
		if (i == 0)
			g_assert_nonnull(strstr(run.out, "Configuration is valid"));
		g_free(run.out);
		g_free(run.err);
		g_assert_false(g_file_test(home_state, G_FILE_TEST_EXISTS));
		g_assert_false(g_file_test(state, G_FILE_TEST_EXISTS));
		g_assert_false(g_file_test(workspace, G_FILE_TEST_EXISTS));
	}
	g_assert_true(g_file_get_contents(path, &after, NULL, NULL));
	g_assert_cmpstr(after, ==, config);
	clawt_test_remove_tree(dir);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/config-offline/files", test_candidate_files);
	g_test_add_func("/config-offline/warnings", test_warning_policy);
	g_test_add_func("/config-offline/cli", test_cli);
	return g_test_run();
}
