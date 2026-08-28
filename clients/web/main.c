/*
 * main.c - clawtilla-web, the web client
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A separate binary rather than a mode of the daemon, deliberately.  This
 * page can start, stop, reconfigure and delete every agent on the machine
 * and run commands inside their computers, so it exists only when
 * somebody has decided they want it -- not because the daemon happened to
 * start.
 */

#include "web-pages.h"

#include <stdlib.h>
#include <string.h>

static gint      opt_port = 8790;
static gchar    *opt_socket = NULL;
static gchar    *opt_profile = NULL;
static gchar    *opt_host = NULL;
static gint      opt_daemon_port = 0;
static gchar    *opt_token = NULL;
static gchar   **opt_bind = NULL;
static gboolean  opt_no_tailscale = FALSE;
static gboolean  opt_version = FALSE;
static gboolean  opt_license = FALSE;

static GOptionEntry entries[] = {
    {
        "port", 'p', 0, G_OPTION_ARG_INT, &opt_port,
        "Port to listen on (default: 8790)", "PORT"
    },
    {
        "bind", 'b', 0, G_OPTION_ARG_STRING_ARRAY, &opt_bind,
        "Address to listen on; repeatable. Replaces the defaults", "ADDR"
    },
    {
        "no-tailscale", 0, 0, G_OPTION_ARG_NONE, &opt_no_tailscale,
        "Do not bind the tailnet address; localhost only", NULL
    },
    {
        "socket", 's', 0, G_OPTION_ARG_FILENAME, &opt_socket,
        "Path to the clawtilla daemon socket", "PATH"
    },
    {
        "profile", 0, 0, G_OPTION_ARG_STRING, &opt_profile,
        "Serve a saved connection instead of the local daemon", "NAME"
    },
    {
        "host", 'H', 0, G_OPTION_ARG_STRING, &opt_host,
        "Serve the daemon at this address instead of the local one", "HOST"
    },
    {
        "daemon-port", 0, 0, G_OPTION_ARG_INT, &opt_daemon_port,
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

static const gchar *license_text =
    "clawtilla-web is part of clawtilla.\n"
    "\n"
    "Copyright (C) 2026 Zach Podbielniak\n"
    "\n"
    "This program is free software: you can redistribute it and/or modify\n"
    "it under the terms of the GNU Affero General Public License as\n"
    "published by the Free Software Foundation, either version 3 of the\n"
    "License, or (at your option) any later version.\n"
    "\n"
    "This program is distributed in the hope that it will be useful, but\n"
    "WITHOUT ANY WARRANTY; without even the implied warranty of\n"
    "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU\n"
    "Affero General Public License for more details.\n"
    "\n"
    "You should have received a copy of the GNU Affero General Public\n"
    "License along with this program.  If not, see\n"
    "<https://www.gnu.org/licenses/>.\n";

/* ── Where the vendored scripts are ──────────────────────────────── */

static gchar *
executable_dir(void)
{
    g_autofree gchar *exe = g_file_read_link("/proc/self/exe", NULL);

    if (exe == NULL)
        return NULL;

    return g_path_get_dirname(exe);
}

/*
 * The directory holding htmx.min.js, best guess first.
 *
 * Beside the binary before the install location, which is the same order
 * clawt-pod-bridge.c settled on and for the same reason: anybody running
 * out of a checkout -- which is everyone until the first `make install`
 * -- has the files right there in `data/web`, and looking only in
 * $PREFIX/share made a fresh clone serve a page with no scripts on it.
 */
static gchar *
find_static_dir(void)
{
    g_autoptr(GPtrArray) tried = g_ptr_array_new_with_free_func(g_free);
    const gchar *override = g_getenv("CLAWT_WEB_STATIC_DIR");
    g_autofree gchar *exe_dir = NULL;
    guint i;

    if (override != NULL)
        g_ptr_array_add(tried, g_strdup(override));

    exe_dir = executable_dir();

    if (exe_dir != NULL) {
        /* build/release/clawtilla-web -> the checkout's data/web */
        g_ptr_array_add(tried, g_build_filename(exe_dir, "..", "..",
                                                "data", "web", NULL));
        g_ptr_array_add(tried, g_build_filename(exe_dir, "web", NULL));
    }

    g_ptr_array_add(tried, g_build_filename(CLAWT_DATA_DIR, "web", NULL));

    for (i = 0; i < tried->len; i++) {
        g_autofree gchar *probe = g_build_filename(
            g_ptr_array_index(tried, i), "htmx.min.js", NULL);

        if (g_file_test(probe, G_FILE_TEST_EXISTS))
            return g_strdup(g_ptr_array_index(tried, i));
    }

    /*
     * Named, all of them. "The scripts are missing" sends somebody to
     * install something; naming the three places it looked sends them to
     * the one that is wrong.
     */
    {
        g_autoptr(GString) places = g_string_new(NULL);

        for (i = 0; i < tried->len; i++)
            g_string_append_printf(places, "\n  %s",
                                   (const gchar *)g_ptr_array_index(tried, i));

        g_warning("clawtilla-web: cannot find htmx.min.js. Looked in:%s\n"
                  "The page will load, but nothing on it will update "
                  "without a reload.", places->str);
    }

    return NULL;
}

/* ── Listening ───────────────────────────────────────────────────── */

/*
 * Binds the addresses this client should answer on.
 *
 * The default is the loopback and the tailnet address, and nothing else.
 * A tailnet is the one network where listening beyond the loopback is
 * defensible without a login of our own: every peer is a device the user
 * enrolled and WireGuard authenticated, and nothing off the tailnet can
 * route to a 100.64/10 address at all. That is the same reasoning
 * clawtillad already uses for its own convenience listener.
 *
 * A machine with no tailnet gets the loopback alone rather than a
 * fallback to every interface. Widening the audience because an address
 * was missing is the opposite of what somebody would want.
 */
static gboolean
bind_addresses(HtmxServer *server, GPtrArray *out_where, GError **error)
{
    guint i;

    if (opt_bind != NULL) {
        for (i = 0; opt_bind[i] != NULL; i++) {
            /*
             * An address a person named is never optional. A client that
             * ignored where it was told to listen and started anyway is
             * running somewhere nobody knows about.
             */
            if (!htmx_server_listen_on(server, opt_bind[i],
                                       (guint16)opt_port, error))
                return FALSE;

            g_ptr_array_add(out_where, g_strdup(opt_bind[i]));
        }

        return TRUE;
    }

    if (!htmx_server_listen_on(server, "127.0.0.1", (guint16)opt_port, error))
        return FALSE;

    g_ptr_array_add(out_where, g_strdup("127.0.0.1"));

    if (opt_no_tailscale)
        return TRUE;

    {
        g_autofree gchar *tailnet = clawt_tailscale_find_address();
        g_autoptr(GError) local = NULL;

        if (tailnet == NULL)
            return TRUE;

        /*
         * A failure here is a warning, not an error. Somebody else
         * holding this port on the tailnet is a reason not to be
         * reachable from a laptop -- it is not a reason to refuse to
         * serve the machine you are sitting at.
         */
        if (!htmx_server_listen_on(server, tailnet, (guint16)opt_port,
                                   &local)) {
            g_warning("clawtilla-web: cannot listen on the tailnet address "
                      "%s: %s", tailnet, local->message);
            return TRUE;
        }

        g_ptr_array_add(out_where, g_steal_pointer(&tailnet));
    }

    return TRUE;
}

/* ── Entry ───────────────────────────────────────────────────────── */

int
main(int argc, char *argv[])
{
    g_autoptr(GOptionContext) context = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(ClawtConnection) connection = NULL;
    g_autoptr(ClawtWebApp) app = NULL;
    g_autoptr(HtmxServer) server = NULL;
    g_autoptr(GMainLoop) loop = NULL;
    g_autoptr(GPtrArray) where = NULL;
    g_autofree gchar *static_dir = NULL;
    HtmxRouter *router;
    GSList *uris;

    context = g_option_context_new("- the clawtilla web client");
    g_option_context_add_main_entries(context, entries, NULL);
    g_option_context_set_description(context,
        "\n"
        "Serves the whole clawtilla client over HTTP: the fleet, chat,\n"
        "the agent inspector, mailboxes, computers, routines, tasks and\n"
        "settings. Everything clawtilla-gtk does.\n"
        "\n"
        "By default it listens on the loopback and, when there is one,\n"
        "this machine's tailnet address -- so a laptop on the same\n"
        "tailnet reaches the fleet without a tunnel set up by hand, and\n"
        "nothing else can reach it at all.\n"
        "\n"
        "It has no login of its own. Do not put it on an interface that\n"
        "is not already authenticated.\n"
        "\n"
        "Examples:\n"
        "  # Loopback and the tailnet, on the default port\n"
        "  clawtilla-web\n"
        "\n"
        "  # This machine only\n"
        "  clawtilla-web --no-tailscale\n"
        "\n"
        "  # Somewhere else entirely, on another port\n"
        "  clawtilla-web --bind 192.168.1.10 --port 9000\n"
        "\n"
        "  # Against a daemon whose socket is not in the usual place\n"
        "  clawtilla-web --socket /run/user/1000/clawtilla/daemon.sock\n"
        "\n"
        "  # Serving a fleet on another machine; no local daemon needed\n"
        "  clawtilla-web --profile workstation\n"
        "  clawtilla-web --host 100.72.0.41 --token \"$(ssh box clawtilla "
        "daemon token)\"\n");

    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        g_printerr("clawtilla-web: %s\n", error->message);
        return EXIT_FAILURE;
    }

    if (opt_version) {
        g_print("clawtilla-web %d.%d.%d (%s)\n",
                CLAWT_VERSION_MAJOR, CLAWT_VERSION_MINOR, CLAWT_VERSION_MICRO,
                CLAWT_GIT_SHA);
        return EXIT_SUCCESS;
    }

    if (opt_license) {
        g_print("%s", license_text);
        return EXIT_SUCCESS;
    }

    if (opt_port <= 0 || opt_port > 65535) {
        g_printerr("clawtilla-web: --port must be between 1 and 65535\n");
        return EXIT_FAILURE;
    }

    /*
     * Which daemon this server serves, decided once at start.
     *
     * Unlike the desktop client there is no switcher: a browser session
     * is not the thing holding the connection, the process is, and
     * repointing it would move every open page at once.  So the choice
     * is a command-line one -- and it has to exist, because a machine
     * serving a fleet over a tailnet need have no fleet of its own.
     * Without these flags clawtilla-web could only ever show the local
     * daemon, and on a laptop that means nothing at all.
     */
    if (opt_host != NULL) {
        connection = clawt_connection_new_remote(
            opt_host, opt_host,
            opt_daemon_port > 0 ? (guint16)opt_daemon_port
                                : CLAWT_DEFAULT_TCP_PORT,
            opt_token);
    } else if (opt_profile != NULL) {
        g_autoptr(GPtrArray) saved = clawt_connection_list_load(NULL, &error);
        ClawtConnection *found;

        if (saved == NULL) {
            g_printerr("clawtilla-web: %s\n", error->message);
            return EXIT_FAILURE;
        }

        found = clawt_connection_list_find(saved, opt_profile);

        if (found == NULL) {
            g_printerr("clawtilla-web: there is no saved connection called "
                       "'%s'\n", opt_profile);
            g_printerr("  `clawtilla remote list` shows the ones there "
                       "are\n");
            return EXIT_FAILURE;
        }

        /* Copied: `saved` owns it and goes out of scope here. */
        connection = clawt_connection_copy(found);
    } else {
        connection = clawt_connection_new_local("Local", opt_socket);
    }

    client = clawt_connection_create_client(connection);
    clawt_client_set_auto_reconnect(client, TRUE);

    /*
     * A server refuses to start when it cannot reach its daemon, where
     * the desktop client opens anyway.  The difference is who is
     * listening: a person at a window can read a banner and pick another
     * machine, and nobody is looking at this one yet.  Saying why on
     * stderr, at the moment somebody ran the command, beats serving a
     * page that says the same thing to a browser that may never arrive.
     */
    if (!clawt_client_connect(client, &error)) {
        g_printerr("clawtilla-web: %s\n", error->message);

        if (clawt_connection_is_local(connection))
            g_printerr("Is clawtillad running?\n");
        else
            g_printerr("  --profile and --host serve a daemon elsewhere; "
                       "`clawtilla remote list` shows the saved ones\n");

        return EXIT_FAILURE;
    }

    /*
     * Subscribed before anything is served, so a browser that connects
     * immediately is already covered by the event stream.
     */
    if (!clawt_client_subscribe(client, 0, NULL, &error)) {
        /*
         * A warning rather than a failure. Without the stream the page
         * stops updating on its own -- which is a worse client, not a
         * broken one, and every action still works because each posts
         * and re-renders.
         */
        g_warning("clawtilla-web: not subscribed to daemon events: %s\n"
                  "Pages will not refresh by themselves.", error->message);
        g_clear_error(&error);
    }

    app = clawt_web_app_new(client);

    /*
     * The connection, so the connections page can show which one is in
     * use and the banner can word itself for a local or a remote daemon.
     */
    clawt_web_app_set_connection(app, connection);

    server = htmx_server_new();
    router = htmx_server_get_router(server);

    static_dir = find_static_dir();

    if (static_dir != NULL)
        htmx_router_serve_static(router, "/static", static_dir);

    clawt_web_register_fleet(router, app);
    clawt_web_register_chat(router, app);
    clawt_web_register_agent(router, app);
    clawt_web_register_mailbox(router, app);
    clawt_web_register_computer(router, app);
    clawt_web_register_work(router, app);
    clawt_web_register_settings(router, app);
    clawt_web_register_events(router, app);
    clawt_web_register_alerts(router, app);
    clawt_web_register_decisions(router, app);
    clawt_web_register_creation(router, app);
    clawt_web_register_extras(router, app);

    /*
     * Last, always: "/a/:id/:view" matches everything under an agent, so
     * anything registered after it is unreachable.
     */
    clawt_web_register_views(router, app);

    where = g_ptr_array_new_with_free_func(g_free);

    if (!bind_addresses(server, where, &error)) {
        g_printerr("clawtilla-web: %s\n", error->message);
        return EXIT_FAILURE;
    }

    /*
     * Reported from what was bound rather than from what was asked for.
     * A convenience address whose bind failed is exactly the interesting
     * case, and announcing the request would say it was reachable there.
     */
    uris = htmx_server_get_listen_uris(server);

    if (uris == NULL) {
        g_printerr("clawtilla-web: nothing is listening\n");
        return EXIT_FAILURE;
    }

    g_print("clawtilla-web is listening on:\n");

    {
        GSList *iter;

        for (iter = uris; iter != NULL; iter = iter->next)
            g_print("  %s\n", (const gchar *)iter->data);
    }

    g_slist_free_full(uris, g_free);

    g_print("\nThere is no login here: anything that can reach a listed\n"
            "address can drive the whole fleet.\n");

    loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);

    htmx_server_stop(server);

    return EXIT_SUCCESS;
}
