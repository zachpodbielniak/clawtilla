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

int
main(int argc, char *argv[])
{
    g_autofree gchar *text = NULL;

    if (argc != 2) {
        g_printerr("usage: clawt-genconfig --example|--default|--org\n");
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
