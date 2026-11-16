/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <clawtilla.h>
#include <glib/gstdio.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include "clawt-test-util.h"

/* Every operation uses a private temporary state tree, never the user's daemon. */
static void
test_roundtrip(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("clawt-backup-XXXXXX", NULL);
	g_autofree gchar *state = g_build_filename(dir, "state", NULL);
	g_autofree gchar *archive = g_build_filename(dir, "archive", NULL);
	g_autofree gchar *dest = g_build_filename(dir, "restored", NULL);
	g_autofree gchar *path = NULL;
	g_autofree gchar *result = NULL;
	g_autofree gchar *contents = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *excluded_names[] = { "token", "tcp-token", ".mcp.json", NULL };
	guint i;
	gsize archive_size;
	g_assert_cmpint(g_mkdir(state, 0700), ==, 0);
	{
		g_autofree gchar *workspace = g_build_filename(state, "agents", "bot", "workspace", NULL);
		g_autofree gchar *project_config = g_build_filename(workspace, "config.yaml", NULL);
		g_autofree gchar *generated = g_build_filename(state, "agents", "bot", "config.yaml", NULL);
		g_assert_cmpint(g_mkdir_with_parents(workspace, 0700), ==, 0);
		g_assert_true(g_file_set_contents(project_config, "project: keep\n", -1, &error));
		g_assert_true(g_file_set_contents(generated, "credential: exclude\n", -1, &error));
	}
	{
		g_autofree gchar *config = g_build_filename(state, "config.yaml", NULL);
		g_autofree gchar *hidden = g_build_filename(state, ".identity", NULL);
		g_assert_true(g_file_set_contents(config, "agents: {}\n", -1, &error));
		g_assert_true(g_file_set_contents(hidden, "persona\n", -1, &error));
	}
	path = g_build_filename(state, "events", NULL);
	g_assert_cmpint(g_mkdir(path, 0700), ==, 0);
	g_free(g_steal_pointer(&path));
	path = g_build_filename(state, "events", "today.ndjson", NULL);
	g_assert_true(g_file_set_contents(path, "event\n", -1, &error));
	for (i = 0; excluded_names[i]; i++) {
		g_autofree gchar *secret = g_build_filename(state, excluded_names[i], NULL);
		g_assert_true(g_file_set_contents(secret, "credential", -1, &error));
	}
	{
		g_autofree gchar *secret_dir = g_build_filename(state, "secrets", NULL);
		g_autofree gchar *secret = g_build_filename(secret_dir, "oauth.json", NULL);
		g_assert_cmpint(g_mkdir(secret_dir, 0700), ==, 0);
		g_assert_true(g_file_set_contents(secret, "credential", -1, &error));
	}
	g_assert_true(clawt_backup_create(state, archive, &error));
	g_assert_no_error(error);
	result = clawt_backup_restore(archive, NULL, &error);
	g_assert_nonnull(strstr(result, "events/today.ndjson\n"));
	g_assert_nonnull(strstr(result, "config.yaml\n"));
	g_assert_nonnull(strstr(result, ".identity\n"));
	g_assert_nonnull(strstr(result, "agents/bot/workspace/config.yaml\n"));
	g_assert_null(strstr(result, "agents/bot/config.yaml\n"));
	g_assert_null(strstr(result, "token"));
	g_assert_null(strstr(result, "oauth"));
	g_assert_null(strstr(result, ".mcp.json"));
	g_assert_false(g_file_test(dest, G_FILE_TEST_EXISTS));
	g_free(g_steal_pointer(&result));
	result = clawt_backup_restore(archive, dest, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_free(g_steal_pointer(&path));
	path = g_build_filename(dest, "events", "today.ndjson", NULL);
	g_assert_true(g_file_get_contents(path, &contents, NULL, &error));
	g_assert_cmpstr(contents, ==, "event\n");
	g_assert_null(clawt_backup_restore(archive, dest, &error));
	g_clear_error(&error);
	g_assert_false(clawt_backup_create(state, archive, &error));
	g_clear_error(&error);
	g_free(g_steal_pointer(&contents));
	g_assert_true(g_file_get_contents(archive, &contents, &archive_size, &error));
	contents[archive_size - 1] ^= 1;
	g_assert_true(g_file_set_contents(archive, contents, (gssize)archive_size, &error));
	g_assert_null(clawt_backup_restore(archive, NULL, &error));
	g_assert_nonnull(strstr(error->message, "SHA256 mismatch"));
	g_clear_error(&error);
	contents[0] = 'X';
	g_assert_true(g_file_set_contents(archive, contents, (gssize)archive_size, &error));
	g_assert_null(clawt_backup_restore(archive, NULL, &error));
	g_assert_nonnull(error);
	clawt_test_remove_tree(dir);
}

/* A separately opened descriptor reproduces the daemon's lifetime flock. */
static void
test_locked_and_symlink(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("clawt-backup-XXXXXX", NULL);
	g_autofree gchar *lock = g_build_filename(dir, "daemon.lock", NULL);
	g_autofree gchar *archive = g_strconcat(dir, ".archive", NULL);
	g_autofree gchar *link = g_build_filename(dir, "escape", NULL);
	g_autoptr(GError) error = NULL;
	gint fd = g_open(lock, O_CREAT | O_RDWR, 0600);
	g_assert_cmpint(fd, >=, 0);
	g_assert_cmpint(flock(fd, LOCK_EX | LOCK_NB), ==, 0);
	g_assert_false(clawt_backup_create(dir, archive, &error));
	g_assert_nonnull(strstr(error->message, "stop the daemon"));
	g_assert_false(g_file_test(archive, G_FILE_TEST_EXISTS));
	g_clear_error(&error);
	close(fd);
	g_assert_cmpint(symlink("/etc", link), ==, 0);
	g_assert_false(clawt_backup_create(dir, archive, &error));
	g_assert_nonnull(strstr(error->message, "symlinks"));
	g_assert_false(g_file_test(archive, G_FILE_TEST_EXISTS));
	clawt_test_remove_tree(dir);
}

/* Recompute the outer checksum so malformed-member tests reach the parser. */
static void
test_hostile_manifest(void)
{
	g_autofree gchar *oversized = g_strnfill(65536, '/');
	const gchar *names[] = { "../escape", "/absolute", "a//b", "secrets/key", "agents/bot/config.yaml", "a\nb", "safe", "duplicate", "parent", oversized, NULL };
	g_autofree gchar *dir = g_dir_make_tmp("clawt-backup-XXXXXX", NULL);
	g_autofree gchar *archive = g_build_filename(dir, "archive", NULL);
	g_autofree gchar *dest = g_build_filename(dir, "restore", NULL);
	guint i;
	for (i = 0; names[i]; i++) {
		GVariantBuilder builder;
		g_autoptr(GVariant) payload = NULL;
		g_autoptr(GBytes) body = NULL;
		g_autofree gchar *sum = NULL;
		g_autoptr(GString) file = NULL;
		g_autoptr(GError) error = NULL;
		g_variant_builder_init(&builder, G_VARIANT_TYPE("a(ssay)"));
		g_variant_builder_add(&builder, "(ss@ay)", names[i], i == 6 ? "bad checksum" : "2d711642b726b04401627ca9fbac32f5c8530fb1903cc4db02258717921a4881",
			g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, "x", 1, 1));
		if (i == 7 || i == 8)
			g_variant_builder_add(&builder, "(ss@ay)", i == 7 ? "duplicate" : "parent/child",
				"2d711642b726b04401627ca9fbac32f5c8530fb1903cc4db02258717921a4881",
				g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, "x", 1, 1));
		payload = g_variant_ref_sink(g_variant_builder_end(&builder));
		body = g_variant_get_data_as_bytes(payload);
		sum = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, body);
		file = g_string_new("CLAWT-BACKUP-1\n");
		g_string_append_printf(file, "%s\n", sum);
		g_string_append_len(file, g_bytes_get_data(body, NULL), g_bytes_get_size(body));
		g_assert_true(g_file_set_contents(archive, file->str, (gssize)file->len, &error));
		g_assert_null(clawt_backup_restore(archive, dest, &error));
		g_assert_nonnull(error);
		g_assert_false(g_file_test(dest, G_FILE_TEST_EXISTS));
	}
	clawt_test_remove_tree(dir);
}

/* Help must dispatch offline, without reaching a socket or a local daemon. */
static void
test_cli(void)
{
	gchar *args[] = { BUILD_OUTDIR "/clawtilla", "backup", "--help", NULL };
	g_autofree gchar *out = NULL;
	g_autofree gchar *err = NULL;
	g_autoptr(GError) error = NULL;
	gint status;
	if (!g_test_subprocess()) {
		g_test_trap_subprocess(NULL, 20 * G_USEC_PER_SEC, 0);
		g_test_trap_assert_passed();
		return;
	}
	g_assert_true(g_spawn_sync(NULL, args, NULL, 0, NULL, NULL, &out, &err, &status, &error));
	g_assert_no_error(error);
	g_assert_true(g_spawn_check_wait_status(status, &error));
	g_assert_nonnull(strstr(out, "backup restore"));
	g_assert_cmpstr(err, ==, "");
	{
		g_autofree gchar *dir = g_dir_make_tmp("clawt-backup-cli-XXXXXX", NULL);
		g_autofree gchar *state = g_build_filename(dir, "state", NULL);
		g_autofree gchar *archive = g_build_filename(dir, "fleet.clawt", NULL);
		g_autofree gchar *dest = g_build_filename(dir, "restored", NULL);
		g_autofree gchar *file = g_build_filename(state, "config.yaml", NULL);
		g_autofree gchar *restored = g_build_filename(dest, "config.yaml", NULL);
		g_autofree gchar *contents = NULL;
		gchar *create[] = { BUILD_OUTDIR "/clawtilla", "backup", "create", state, archive, NULL };
		gchar *preview[] = { BUILD_OUTDIR "/clawtilla", "backup", "preview", archive, NULL };
		gchar *restore[] = { BUILD_OUTDIR "/clawtilla", "backup", "restore", archive, dest, NULL };
		gchar **commands[] = { create, preview, restore };
		guint i;
		g_assert_cmpint(g_mkdir(state, 0700), ==, 0);
		g_assert_true(g_file_set_contents(file, "agents: []\n", -1, &error));
		for (i = 0; i < G_N_ELEMENTS(commands); i++) {
			g_clear_pointer(&out, g_free);
			g_clear_pointer(&err, g_free);
			g_assert_true(g_spawn_sync(NULL, commands[i], NULL, 0, NULL, NULL, &out, &err, &status, &error));
			g_assert_no_error(error);
			g_assert_true(g_spawn_check_wait_status(status, &error));
			g_assert_no_error(error);
			g_assert_cmpstr(err, ==, "");
			if (i == 1) {
				g_assert_nonnull(strstr(out, "config.yaml"));
				g_assert_false(g_file_test(dest, G_FILE_TEST_EXISTS));
			}
		}
		g_assert_true(g_file_get_contents(restored, &contents, NULL, &error));
		g_assert_cmpstr(contents, ==, "agents: []\n");
		clawt_test_remove_tree(dir);
	}
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/backup/roundtrip", test_roundtrip);
	g_test_add_func("/backup/locked-symlink", test_locked_and_symlink);
	g_test_add_func("/backup/hostile-manifest", test_hostile_manifest);
	g_test_add_func("/backup/cli", test_cli);
	return g_test_run();
}
