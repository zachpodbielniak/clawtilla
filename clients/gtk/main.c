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
        "open", 'o', 0, G_OPTION_ARG_STRING, &opt_open,
        "Open an agent's chat, a peer conversation or a group room "
        "(in the running window, if there is one)", "ROOM"
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
    "  # Open a conversation: an agent's chat, a group room, or a\n"
    "  # direct room between two agents.  `clawtilla:<room>` works too,\n"
    "  # which is what an org-mode link hands over\n"
    "  clawtilla-gtk --open chief\n"
    "  clawtilla-gtk --open standup\n"
    "  clawtilla-gtk --open clawtilla:dm:chief:lead\n"
    "\n"
    "--open goes to the window that is already running when there is\n"
    "one (the app is single-instance), and a name the daemon does not\n"
    "know is reported in the window rather than opened.\n"
    "\n"
    "A local daemon is started with `clawtillad --foreground`, or by the\n"
    "systemd --user service. It does not have to be running: the window\n"
    "opens either way, keeps trying, and the connection menu left of the\n"
    "app title switches to any daemon saved with `clawtilla remote add`.\n";

static void
on_activate(AdwApplication *app, gpointer user_data)
{
    ClawtClient *client = user_data;
    ClawtConnection *connection =
        g_object_get_data(G_OBJECT(app), "connection");
    ClawtWindow *window;

    window = clawt_window_new(app, client, connection);
    gtk_window_present(GTK_WINDOW(window));

    /*
     * This process's own --open, once there is a window to open it in.
     * Taken, so a second activation (another invocation with no
     * --open) does not reopen it over whatever is on screen by then.
     */
    if (opt_open != NULL) {
        g_autofree gchar *target = g_steal_pointer(&opt_open);

        clawt_window_open_target(window, target);
    }
}

/*
 * Opens @target in the newest window, the one somebody last used; with
 * no window yet (activate has not run) the target waits for it.
 */
static void
open_in_app(GtkApplication *app, const gchar *target)
{
    GtkWindow *window = gtk_application_get_active_window(app);

    if (window == NULL || !CLAWT_IS_WINDOW(window)) {
        g_free(opt_open);
        opt_open = g_strdup(target);
        g_application_activate(G_APPLICATION(app));
        return;
    }

    clawt_window_open_target(CLAWT_WINDOW(window), target);
}

/*
 * Every invocation's command line, run in the primary instance.
 *
 * This process's own arrives with its options already parsed by main()
 * (it is handed argv[0] alone), so it simply activates.  Another
 * invocation's arrives whole: a `--open` in it opens in the window
 * already on screen, and anything else opens a window as a second
 * `clawtilla-gtk` always has.  Only `--open` is read here; the rest
 * were checked in the process that typed them.
 */
static gint
on_command_line(GApplication *app, GApplicationCommandLine *cmdline,
                gpointer user_data)
{
    g_autofree gchar *target = NULL;
    g_auto(GStrv) argv = NULL;

    (void)user_data;

    if (!g_application_command_line_get_is_remote(cmdline)) {
        g_application_activate(app);
        return EXIT_SUCCESS;
    }

    argv = g_application_command_line_get_arguments(cmdline, NULL);
    target = clawt_open_target_from_argv((const gchar * const *)argv);

    if (target == NULL) {
        g_application_activate(app);
        return EXIT_SUCCESS;
    }

    open_in_app(GTK_APPLICATION(app), target);

    return EXIT_SUCCESS;
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

    /*
     * The spelling is checked here, where the person who typed it is
     * looking, so only a well-formed target is ever forwarded; whether
     * the room exists needs the daemon and is the window's to say.
     */
    if (opt_open != NULL) {
        g_autofree gchar *target = clawt_open_target_normalize(opt_open,
                                                               &error);

        if (target == NULL) {
            g_printerr("clawtilla-gtk: --open %s: %s\n", opt_open,
                       error->message);
            return EXIT_FAILURE;
        }

        g_free(opt_open);
        opt_open = g_steal_pointer(&target);
    }

    app = adw_application_new("org.clawtilla.Gtk",
                              G_APPLICATION_HANDLES_COMMAND_LINE);
    g_signal_connect(app, "command-line", G_CALLBACK(on_command_line), NULL);

    /*
     * One window answers links.  Registered before anything connects,
     * so a second `clawtilla-gtk --open` hands its target to the first
     * over D-Bus and exits without opening a daemon connection of its
     * own -- which is what makes an org link click land in the window
     * already on screen rather than in a new one.  A registration that
     * fails (no session bus) is not fatal: the process carries on as
     * its own primary instance.
     */
    if (!g_application_register(G_APPLICATION(app), NULL, &error)) {
        g_printerr("clawtilla-gtk: %s\n", error->message);
        g_clear_error(&error);
    } else if (opt_open != NULL &&
               g_application_get_is_remote(G_APPLICATION(app))) {
        /*
         * Through g_application_run(), which sends the command line,
         * waits for the primary to answer it and takes the bus
         * registration down afterwards -- the last of which a bare
         * D-Bus call followed by an exit does not.
         */
        gchar *forward[] = { argv[0], (gchar *)"--open", opt_open, NULL };

        status = g_application_run(G_APPLICATION(app), 3, forward);
        g_clear_pointer(&opt_open, g_free);
        return status;
    }

    /*
     * Which daemon to open on: --host, then --profile, then --socket,
     * then the one this client was last switched to, then the local
     * socket.  A laptop with no fleet of its own starts straight on the
     * workstation it was using, rather than on "the daemon is not
     * running" for a daemon it was never asking about.
     */
    connection = clawt_connection_choose(
        opt_host, opt_port > 0 ? (guint16)opt_port : 0, opt_token,
        opt_profile, opt_socket_path, NULL, NULL, TRUE, &error);

    if (connection == NULL) {
        g_printerr("clawtilla-gtk: %s\n", error->message);
        return EXIT_FAILURE;
    }

    client = clawt_connection_create_client(connection);

    /*
     * Reconnection is on: the daemon restarts whenever its configuration
     * changes, and a desktop client that gave up on the first drop would
     * sit there showing a fleet that had moved on.
     */
    clawt_client_set_auto_reconnect(client, TRUE);

    /*
     * A daemon that is not there is not a reason to refuse to start.
     *
     * This used to put up a status page saying "the daemon is not
     * running" and nothing else -- no connection menu, no way to reach
     * any of the machines in connections.yaml, on the one screen where
     * somebody most needs them.  A laptop with no fleet of its own could
     * therefore not open a workstation's fleet at all unless it named it
     * on the command line, which is exactly backwards: the local daemon
     * is the connection least likely to matter to a client whose whole
     * point is reaching daemons elsewhere.
     *
     * So the window opens either way.  It says what happened in its own
     * banner, offers the connection menu, and keeps trying in the
     * background -- which also means a person who starts clawtillad a
     * moment later finds the window has filled itself in rather than
     * having to close it and open it again.
     *
     * The failure is still reported on stderr.  Somebody who typed
     * --host and got the wrong port wants to see the reason without
     * hunting for it in a banner.
     */
    if (!clawt_client_connect(client, &error)) {
        g_printerr("clawtilla-gtk: %s\n", error->message);
        clawt_client_start_reconnecting(client);
    }

    /*
     * Asked for unconditionally.  The subscription is an intent the
     * client remembers, so a window that came up before its daemon is
     * subscribed the moment the daemon appears -- rather than
     * reconnecting to a live socket and receiving nothing for ever.
     */
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
