/*
 * main.c - clawtilla-lrg, the libregnum desktop client
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Same options and the same choice of daemon as clawtilla-gtk: --host
 * wins over --profile, which wins over the local socket, and a daemon
 * that is not there is not a reason to refuse to start.
 */

#include "clawt-lrg.h"

#include <stdlib.h>

static gchar *opt_socket_path = NULL;
static gchar *opt_profile = NULL;
static gchar *opt_host = NULL;
static gint opt_port = 0;
static gchar *opt_token = NULL;
static gchar *opt_self_test = NULL;
static gboolean opt_version = FALSE;
static gboolean opt_license = FALSE;

static GOptionEntry entries[] = {
    {
        "socket", 's', 0, G_OPTION_ARG_FILENAME, &opt_socket_path,
        "Path to the daemon's socket", "FILE"
    },
    {
        "profile", 'p', 0, G_OPTION_ARG_STRING, &opt_profile,
        "Start on a saved connection", "NAME"
    },
    {
        "host", 'H', 0, G_OPTION_ARG_STRING, &opt_host,
        "Start on a daemon at this address", "HOST"
    },
    {
        "port", 0, 0, G_OPTION_ARG_INT, &opt_port,
        "Port for --host (default 8792)", "PORT"
    },
    {
        "token", 0, 0, G_OPTION_ARG_STRING, &opt_token,
        "Bearer token for --host", "TOKEN"
    },
    {
        "self-test", 0, 0, G_OPTION_ARG_STRING, &opt_self_test,
        "Lay out one page without a window, print its widget tree as JSON "
        "and exit (for tests)", "PAGE"
    },
    {
        "version", 'V', 0, G_OPTION_ARG_NONE, &opt_version,
        "Print version information and exit", NULL
    },
    {
        "license", 0, 0, G_OPTION_ARG_NONE, &opt_license,
        "Print licensing information and exit", NULL
    },
    { NULL }
};

static const gchar *description_text =
    "\n"
    "Examples:\n"
    "  # Connect to the running daemon\n"
    "  clawtilla-lrg\n"
    "\n"
    "  # Connect to a daemon on a different socket\n"
    "  clawtilla-lrg --socket /run/user/1000/clawtilla/daemon.sock\n"
    "\n"
    "  # Start on a connection saved with `clawtilla remote add`\n"
    "  clawtilla-lrg --profile workstation\n"
    "\n"
    "  # Start on a daemon reached over a tailnet\n"
    "  clawtilla-lrg --host 100.72.0.41 --token \"$(ssh box clawtilla "
    "daemon token)\"\n"
    "\n"
    "  # Print the Chat page's widget tree, for a test script\n"
    "  clawtilla-lrg --self-test chat\n"
    "\n"
    "Appearance is read from ~/.config/clawtilla/appearance.yaml, shared\n"
    "with clawtilla-gtk; the lrg_* keys there are this client's own\n"
    "(motion, material, background, accent, scale). See\n"
    "docs/lrg-client.org.\n";

static const gchar *license_text =
    "clawtilla-lrg - the clawtilla libregnum client\n"
    "Copyright (C) 2026\n"
    "\n"
    "This program is free software: you can redistribute it and/or\n"
    "modify it under the terms of the GNU Affero General Public\n"
    "License as published by the Free Software Foundation, either\n"
    "version 3 of the License, or (at your option) any later\n"
    "version.\n"
    "\n"
    "This program is distributed in the hope that it will be\n"
    "useful, but WITHOUT ANY WARRANTY; without even the implied\n"
    "warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR\n"
    "PURPOSE.  See the GNU Affero General Public License for more\n"
    "details.\n"
    "\n"
    "You should have received a copy of the GNU Affero General\n"
    "Public License along with this program.  If not, see\n"
    "<https://www.gnu.org/licenses/>.\n";

/* Which daemon to open on: --host, then --profile, then the local socket. */
static ClawtConnection *
choose_connection(GError **error)
{
    if (opt_host != NULL)
        return clawt_connection_new_remote(
            opt_host, opt_host,
            opt_port > 0 ? (guint16)opt_port : CLAWT_DEFAULT_TCP_PORT,
            opt_token);

    if (opt_profile != NULL) {
        g_autoptr(GPtrArray) saved = clawt_connection_list_load(NULL, error);
        ClawtConnection *found;

        if (saved == NULL)
            return NULL;

        found = clawt_connection_list_find(saved, opt_profile);

        if (found == NULL) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                        "there is no saved connection called '%s'",
                        opt_profile);
            return NULL;
        }

        /* copied: `saved` owns it and goes out of scope here */
        return clawt_connection_copy(found);
    }

    return clawt_connection_new_local("Local", opt_socket_path);
}

int
main(int argc, char *argv[])
{
    g_autoptr(GOptionContext) context = NULL;
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(ClawtConnection) connection = NULL;
    g_autoptr(GError) error = NULL;
    ClawtLrgApp *app;
    gint status;

    context = g_option_context_new("- the clawtilla libregnum client");
    g_option_context_add_main_entries(context, entries, NULL);
    g_option_context_set_description(context, description_text);

    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        g_printerr("%s: %s\n", CLAWT_LRG_BIN_NAME, error->message);
        return EXIT_FAILURE;
    }

    if (opt_version) {
        g_print("%s %s (%s)\n", CLAWT_LRG_BIN_NAME, CLAWT_VERSION_STRING,
                CLAWT_GIT_SHA);
        return EXIT_SUCCESS;
    }

    if (opt_license) {
        g_print("%s", license_text);
        return EXIT_SUCCESS;
    }

    connection = choose_connection(&error);

    if (connection == NULL) {
        g_printerr("%s: %s\n", CLAWT_LRG_BIN_NAME, error->message);
        return EXIT_FAILURE;
    }

    client = clawt_connection_create_client(connection);
    app = clawt_lrg_app_new(client, connection);
    status = clawt_lrg_app_run(app, opt_self_test);
    clawt_lrg_app_free(app);

    return status;
}
