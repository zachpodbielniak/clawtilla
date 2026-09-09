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
	return g_test_run();
}
