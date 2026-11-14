/*
 * test-connector-secrets.c - Where a connector's credential lives
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A confined agent's box binds each granted connector's own directory
 * under secrets.dir/connectors/, so a credential still in the old flat
 * layout (secrets.dir/connector-<name>.json) is moved there by the
 * daemon when it starts, before any agent's box is built -- and the
 * configuration is pointed at the new file.  A token_file somebody chose
 * is left exactly where it is.  Hermetic: a daemon on a private context,
 * a unix socket, no network.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>
#include <string.h>

#include "clawt-test-util.h"

static void
write_token(
	const gchar *path,
	const gchar *value
){
	g_autofree gchar *parent = g_path_get_dirname(path);
	g_autofree gchar *text = g_strdup_printf("{\"access_token\":\"%s\"}",
	                                         value);

	g_assert_cmpint(g_mkdir_with_parents(parent, 0700), ==, 0);
	g_assert_true(g_file_set_contents(path, text, -1, NULL));
	g_assert_cmpint(g_chmod(path, 0600), ==, 0);
}

static void
test_a_flat_credential_moves_at_start(
	void
){
	g_autofree gchar *made = g_dir_make_tmp("clawt-conn-secrets-XXXXXX",
	                                        NULL);
	g_autofree gchar *dir = clawt_canonicalize_missing(made);
	g_autofree gchar *secrets = g_build_filename(dir, "secrets", NULL);
	g_autofree gchar *config_path = g_build_filename(dir, "config.yaml",
	                                                 NULL);
	g_autofree gchar *flat = NULL;
	g_autofree gchar *moved = NULL;
	g_autofree gchar *chosen = NULL;
	g_autofree gchar *yaml = NULL;
	g_autofree gchar *saved = NULL;
	g_autofree gchar *contents = NULL;
	g_autofree gchar *token_dir = NULL;
	g_autoptr(GMainContext) context = NULL;
	g_autoptr(ClawtDaemon) daemon = NULL;
	g_autoptr(GError) error = NULL;
	GStatBuf st;

	flat = clawt_connector_legacy_token_path(secrets, "work");
	moved = clawt_connector_token_path(secrets, "work");
	token_dir = clawt_connector_token_dir(secrets, "work");
	chosen = g_build_filename(dir, "keys", "mine.json", NULL);
	write_token(flat, "flat-credential");
	write_token(chosen, "chosen-credential");

	yaml = g_strdup_printf(
		"daemon:\n  tailscale: false\n  state_dir: \"%s/state\"\n"
		"  socket: \"%s/d.sock\"\n  automation_dir: \"%s/pods\"\n"
		"secrets:\n  dir: \"%s\"\n"
		"skills:\n  dir: \"%s/skills\"\n  extra_dirs: []\n"
		"defaults:\n  workspace_root: \"%s/agents\"\n"
		"connectors:\n  dir: \"%s/connectors.d\"\n"
		"integrations:\n"
		"  - name: work\n"
		"    type: connector\n"
		"    provider: bearer\n"
		"    url: https://mcp.example.com/work\n"
		"    token_file: \"%s\"\n"
		"    scope: all\n"
		"  - name: mine\n"
		"    type: connector\n"
		"    provider: bearer\n"
		"    url: https://mcp.example.com/mine\n"
		"    token_file: \"%s\"\n"
		"    scope: all\n"
		"agents:\n"
		"  - id: kudu\n    runtime:\n      autostart: false\n",
		dir, dir, dir, secrets, dir, dir, dir, flat, chosen);
	g_assert_true(g_file_set_contents(config_path, yaml, -1, &error));

	context = g_main_context_new();
	g_main_context_push_thread_default(context);
	daemon = clawt_daemon_new(config_path, context);
	g_assert_true(clawt_daemon_start(daemon, &error));
	g_assert_no_error(error);

	/* Moved whole, the flat copy gone, the directory the daemon's alone. */
	g_assert_false(g_file_test(flat, G_FILE_TEST_EXISTS));
	g_assert_true(g_file_get_contents(moved, &contents, NULL, NULL));
	g_assert_nonnull(strstr(contents, "flat-credential"));
	g_assert_cmpint(g_stat(token_dir, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0700);

	/* The configuration names the new file, and only the new file. */
	g_assert_true(g_file_get_contents(config_path, &saved, NULL, NULL));
	g_assert_nonnull(strstr(saved, moved));
	g_assert_null(strstr(saved, flat));

	/* A path somebody chose is theirs, and stays. */
	g_assert_true(g_file_test(chosen, G_FILE_TEST_IS_REGULAR));
	g_assert_nonnull(strstr(saved, chosen));

	clawt_daemon_stop(daemon);

	while (g_main_context_iteration(context, FALSE))
		;

	g_main_context_pop_thread_default(context);
	g_clear_object(&daemon);
	clawt_test_remove_tree(dir);
}

int
main(
	int   argc,
	char *argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/connector-secrets/flat-credential-moves",
	                test_a_flat_credential_moves_at_start);

	return g_test_run();
}
