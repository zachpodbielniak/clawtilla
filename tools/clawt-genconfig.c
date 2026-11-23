/*
 * clawt-genconfig.c - Generates the config files and docs from the schema
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Build-time tool.  Run by `make config-files`; the output is checked in so
 * a reader browsing the repository sees real content and a test can compare
 * against it.
 */

#include <clawtilla.h>
#include "config/clawt-config-schema.h"

#include <stdlib.h>

/*
 * --skill DIR: the /clawtilla skill's configuration reference, which is
 * a directory rather than one file -- an index and a file per top-level
 * section -- so it is written here rather than through stdout.
 */
static int
write_skill(const gchar *dir)
{
    g_autofree gchar *index_text = NULL;
    g_autofree gchar *index_path = NULL;
    g_autofree gchar *section_dir = NULL;
    g_auto(GStrv) sections = NULL;
    g_autoptr(GError) error = NULL;
    guint i;

    section_dir = g_build_filename(dir, "config", NULL);
    if (g_mkdir_with_parents(section_dir, 0755) != 0) {
        g_printerr("clawt-genconfig: cannot create %s\n", section_dir);
        return EXIT_FAILURE;
    }

    index_text = clawt_config_schema_render_skill_index();
    index_path = g_build_filename(dir, "config-keys.md", NULL);
    if (!g_file_set_contents(index_path, index_text, -1, &error)) {
        g_printerr("clawt-genconfig: %s\n", error->message);
        return EXIT_FAILURE;
    }

    sections = clawt_config_schema_skill_sections();
    for (i = 0; sections[i] != NULL; i++) {
        g_autofree gchar *text = clawt_config_schema_render_skill_section(sections[i]);
        g_autofree gchar *name = g_strconcat(sections[i], ".md", NULL);
        g_autofree gchar *path = g_build_filename(section_dir, name, NULL);

        if (!g_file_set_contents(path, text, -1, &error)) {
            g_printerr("clawt-genconfig: %s\n", error->message);
            return EXIT_FAILURE;
        }
    }

    return EXIT_SUCCESS;
}

int
main(int argc, char *argv[])
{
    g_autofree gchar *text = NULL;

    if (argc == 3 && g_strcmp0(argv[1], "--skill") == 0)
        return write_skill(argv[2]);

    if (argc != 2) {
        g_printerr("usage: clawt-genconfig --example|--default|--org|--skill DIR\n");
        return EXIT_FAILURE;
    }

    if (g_strcmp0(argv[1], "--example") == 0)
        text = clawt_config_schema_render_example();
    else if (g_strcmp0(argv[1], "--default") == 0)
        text = clawt_config_schema_render_default();
    else if (g_strcmp0(argv[1], "--org") == 0)
        text = clawt_config_schema_render_org();
    else {
        g_printerr("clawt-genconfig: unknown mode '%s'\n", argv[1]);
        return EXIT_FAILURE;
    }

    g_print("%s", text);
    return EXIT_SUCCESS;
}
