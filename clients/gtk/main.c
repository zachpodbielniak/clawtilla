/*
 * main.c - clawtilla-gtk, the desktop client
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Deliberately thin, like the daemon's own main: the client holds no
 * transports and no state of its own, only a ClawtClient.  Everything it
 * shows comes from the daemon, so two clients open at once agree with
 * each other rather than each having their own idea of the fleet.
 */

#include "clawt-gtk.h"

#include <stdlib.h>

static gchar *opt_socket_path = NULL;
static gchar *opt_profile = NULL;
static gchar *opt_host = NULL;
static gint opt_port = 0;
static gchar *opt_token = NULL;
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
    "  clawtilla-gtk\n"
    "\n"
    "  # Connect to a daemon on a different socket\n"
    "  clawtilla-gtk --socket /run/user/1000/clawtilla/daemon.sock\n"
    "\n"
    "  # Start on a connection saved from the app's connection menu\n"
    "  clawtilla-gtk --profile workstation\n"
    "\n"
    "  # Start on a daemon reached over a tailnet\n"
    "  clawtilla-gtk --host 100.72.0.41 --token \"$(ssh box clawtilla "
    "daemon token)\"\n"
    "\n"
    "The daemon must already be running: start it with `clawtillad\n"
    "--foreground`, or enable the systemd --user service. Connections can\n"
    "also be saved and switched from the menu left of the app title.\n";

static void
on_activate(AdwApplication *app, gpointer user_data)
{
    ClawtClient *client = user_data;
    ClawtConnection *connection =
        g_object_get_data(G_OBJECT(app), "connection");
    ClawtWindow *window;

    window = clawt_window_new(app, client, connection);
    gtk_window_present(GTK_WINDOW(window));
}

/*
 * Shown instead of the main window when the daemon cannot be reached.
 *
 * An empty window with a toast would look like a fleet with no agents in
 * it, which is a different problem with a different fix.
 */
static void
show_not_running(AdwApplication *app, gpointer user_data)
{
    const gchar *reason = user_data;
    GtkWidget *window = adw_application_window_new(GTK_APPLICATION(app));
    GtkWidget *status = adw_status_page_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    adw_status_page_set_icon_name(ADW_STATUS_PAGE(status),
                                  "network-offline-symbolic");
    adw_status_page_set_title(ADW_STATUS_PAGE(status),
                              "The daemon is not running");
    adw_status_page_set_description(ADW_STATUS_PAGE(status), reason);

    gtk_box_append(GTK_BOX(box), adw_header_bar_new());
    gtk_box_append(GTK_BOX(box), status);
    gtk_widget_set_vexpand(status, TRUE);

    adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), box);
    gtk_window_set_default_size(GTK_WINDOW(window), 560, 400);
    gtk_window_present(GTK_WINDOW(window));
}

int
main(int argc, char *argv[])
{
    g_autoptr(GOptionContext) context = NULL;
    g_autoptr(AdwApplication) app = NULL;
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(ClawtConnection) connection = NULL;
    g_autoptr(GError) error = NULL;
    gint status;

    context = g_option_context_new("- the clawtilla desktop client");
    g_option_context_add_main_entries(context, entries, NULL);
    g_option_context_set_description(context, description_text);

    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        g_printerr("clawtilla-gtk: %s\n", error->message);
        return EXIT_FAILURE;
    }

    if (opt_version) {
        g_print("clawtilla-gtk %s (%s)\n", CLAWT_VERSION_STRING,
                CLAWT_GIT_SHA);
        return EXIT_SUCCESS;
    }

    if (opt_license) {
        g_print(
            "clawtilla-gtk - the clawtilla desktop client\n"
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
            "<https://www.gnu.org/licenses/>.\n");
        return EXIT_SUCCESS;
    }

    app = adw_application_new("org.clawtilla.Gtk", G_APPLICATION_DEFAULT_FLAGS);

    /*
     * Which daemon to open on.  --host wins over --profile, which wins
     * over the local socket; a laptop with no fleet of its own can start
     * straight on a workstation rather than being shown "the daemon is
     * not running" for a daemon it was never asking about.
     */
    if (opt_host != NULL) {
        connection = clawt_connection_new_remote(
            opt_host, opt_host,
            opt_port > 0 ? (guint16)opt_port : CLAWT_DEFAULT_TCP_PORT,
            opt_token);
    } else if (opt_profile != NULL) {
        g_autoptr(GPtrArray) saved = clawt_connection_list_load(NULL, &error);

        if (saved == NULL) {
            g_printerr("clawtilla-gtk: %s\n", error->message);
            return EXIT_FAILURE;
        }

        connection = clawt_connection_list_find(saved, opt_profile);

        if (connection == NULL) {
            g_printerr("clawtilla-gtk: there is no saved connection called "
                       "'%s'\n", opt_profile);
            return EXIT_FAILURE;
        }

        /* Copied: `saved` owns it and goes out of scope here. */
        connection = clawt_connection_copy(connection);
    } else {
        connection = clawt_connection_new_local("Local", opt_socket_path);
    }

    client = clawt_connection_create_client(connection);

    /*
     * Reconnection is on: the daemon restarts whenever its configuration
     * changes, and a desktop client that gave up on the first drop would
     * sit there showing a fleet that had moved on.
     */
    clawt_client_set_auto_reconnect(client, TRUE);

    if (!clawt_client_connect(client, &error)) {
        /*
         * Kept alive on the application rather than freed here: the
         * activate handler runs after this scope ends, and a message
         * freed before it is read would print whatever is now at that
         * address.
         */
        g_object_set_data_full(G_OBJECT(app), "reason",
                               g_strdup(error->message), g_free);

        g_signal_connect(app, "activate", G_CALLBACK(show_not_running),
                         g_object_get_data(G_OBJECT(app), "reason"));

        return g_application_run(G_APPLICATION(app), 1, argv);
    }

    clawt_client_subscribe(client, 0, NULL, NULL);

    /*
     * Kept on the application for the same reason the failure message is:
     * activate runs after this scope ends.
     */
    g_object_set_data_full(G_OBJECT(app), "connection",
                           clawt_connection_copy(connection),
                           (GDestroyNotify)clawt_connection_free);

    g_signal_connect(app, "activate", G_CALLBACK(on_activate), client);

    status = g_application_run(G_APPLICATION(app), 1, argv);

    return status;
}
