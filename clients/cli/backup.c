/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <clawtilla.h>
#include "backup.h"
#include <string.h>

/* Offline verbs never connect to the daemon or initialize its state. */
gint
clawt_cli_backup(gint argc, gchar **argv)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *manifest = NULL;
	gboolean ok = FALSE;
	if (argc == 3 && (g_str_equal(argv[2], "--help") || g_str_equal(argv[2], "-h"))) {
		g_print("Offline backup (stop the daemon first):\n"
			"  clawtilla backup create STATE_DIR ARCHIVE\n"
			"  clawtilla backup verify ARCHIVE\n"
			"  clawtilla backup preview ARCHIVE\n"
			"  clawtilla backup restore ARCHIVE NEW_DIRECTORY\n"
			"Examples:\n"
			"  clawtilla backup create ~/.clawtilla ~/fleet.clawt\n"
			"  clawtilla backup preview ~/fleet.clawt\n"
			"  clawtilla backup restore ~/fleet.clawt ~/restored-fleet\n"
			"Includes fleet config.yaml; excludes generated configs, credentials, secrets, tokens, sessions, .git, .env, .ssh and .mcp.json.\n"
			"256 MiB archive / 64 MiB file limits; symlinks refused. See docs/maintenance.org.\n");
		return 0;
	}
	if (argc == 3 && g_str_equal(argv[2], "--license")) {
		g_print("SPDX-License-Identifier: AGPL-3.0-or-later\n");
		return 0;
	}
	if (argc == 5 && g_str_equal(argv[2], "create"))
		ok = clawt_backup_create(argv[3], argv[4], &error);
	else if ((argc == 4 && (g_str_equal(argv[2], "verify") || g_str_equal(argv[2], "preview"))) ||
		(argc == 5 && g_str_equal(argv[2], "restore"))) {
		manifest = clawt_backup_restore(argv[3], argc == 5 ? argv[4] : NULL, &error);
		ok = manifest != NULL;
		if (ok)
			g_print("%s", manifest);
	} else {
		g_printerr("Usage: clawtilla backup --help\n");
		return 2;
	}
	if (!ok) {
		g_printerr("backup: %s\n", error->message);
		return 1;
	}
	return 0;
}
