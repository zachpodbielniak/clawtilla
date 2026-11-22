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
static gboolean opt_office = FALSE;
static gchar *opt_host = NULL;
static gint opt_port = 0;
static gchar *opt_token = NULL;
static gchar *opt_self_test = NULL;
static gboolean opt_version = FALSE;
static gboolean opt_license = FALSE;
static gchar *opt_open = NULL;

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
        "open", 'o', 0, G_OPTION_ARG_STRING, &opt_open,
        "Open on an agent's chat, a peer conversation or a group room",
        "ROOM"
    },
    {
        "office", 0, 0, G_OPTION_ARG_NONE, &opt_office,
        "Open on the Office page: who is where, and the office drawn", NULL
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
    "  # Open on the Office page\n"
    "  clawtilla-lrg --office\n"
    "\n"
    "  # Start on a daemon reached over a tailnet\n"
    "  clawtilla-lrg --host 100.72.0.41 --token \"$(ssh box clawtilla "
    "daemon token)\"\n"
    "\n"
    "  # Open on a conversation: an agent's chat, a group room, or a\n"
    "  # direct room between two agents (`clawtilla:<room>` works too)\n"
    "  clawtilla-lrg --open chief\n"
    "  clawtilla-lrg --open clawtilla:dm:chief:lead\n"
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

/*
 * Which daemon to open on: --host, then --profile, then --socket, then
 * the one this client was last switched to, then the local socket.  The
 * self-test never remembers: it runs against what it was told.
 */
static ClawtConnection *
choose_connection(gboolean remembered, gboolean *from_last, gchar **notice,
                  GError **error)
{
    return clawt_connection_choose(
        opt_host, opt_port > 0 ? (guint16)opt_port : 0, opt_token,
        opt_profile, opt_socket_path, NULL, NULL, remembered, from_last,
        notice, error);
}

int
main(int argc, char *argv[])
{
    g_autoptr(GOptionContext) context = NULL;
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(ClawtConnection) connection = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *notice = NULL;
    gboolean from_last = FALSE;
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

    /*
     * The spelling is checked here, where the person who typed it is
     * looking; whether the room exists needs the daemon and is the
     * window's to say, as a toast, once the fleet has been read.
     */
    if (opt_open != NULL) {
        g_autofree gchar *target = clawt_open_target_normalize(opt_open,
                                                               &error);

        if (target == NULL) {
            g_printerr("%s: --open %s: %s\n", CLAWT_LRG_BIN_NAME, opt_open,
                       error->message);
            return EXIT_FAILURE;
        }

        g_free(opt_open);
        opt_open = g_steal_pointer(&target);
    }

    connection = choose_connection(opt_self_test == NULL, &from_last,
                                   &notice, &error);

    if (connection == NULL) {
        g_printerr("%s: %s\n", CLAWT_LRG_BIN_NAME, error->message);
        return EXIT_FAILURE;
    }

    client = clawt_connection_create_client(connection);
    app = clawt_lrg_app_new(client, connection);
    app->open_target = g_steal_pointer(&opt_open);
    app->open_office = opt_office;

    /*
     * Whether the daemon came from memory (the banner's Use Local) and
     * what choosing it had to say (a toast once the window is up; on
     * stderr too, like a connect that fails).
     */
    app->connection_remembered = from_last;

    if (notice != NULL) {
        g_printerr("%s: %s\n", CLAWT_LRG_BIN_NAME, notice);
        app->start_notice = g_steal_pointer(&notice);
    }
    status = clawt_lrg_app_run(app, opt_self_test);
    clawt_lrg_app_free(app);

    return status;
}
