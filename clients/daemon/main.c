/*
 * main.c - clawtillad, the clawtilla daemon
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Deliberately thin.  Everything the daemon does lives in libclawt behind
 * ClawtDaemon, so that an in-process host -- cmacs, most immediately -- can
 * get the identical daemon by calling clawt_daemon_new() and owning the main
 * loop itself, without a socket or a second process in the way.
 *
 * The rule this file exists to preserve: clients hold no transports.  Every
 * agent process, every credential and every socket belongs to the daemon.
 */

#include <clawtilla.h>

#include <glib-unix.h>

#include <stdlib.h>

static gboolean opt_foreground = FALSE;
static gboolean opt_version = FALSE;
static gchar   *opt_config_path = NULL;

static GOptionEntry entries[] = {
    {
        "config", 'c', 0, G_OPTION_ARG_FILENAME, &opt_config_path,
        "Path to the clawtilla configuration YAML", "FILE"
    },
    {
        "foreground", 'f', 0, G_OPTION_ARG_NONE, &opt_foreground,
        "Stay in the foreground and log to stderr", NULL
    },
    {
        "version", 'V', 0, G_OPTION_ARG_NONE, &opt_version,
        "Print version information and exit", NULL
    },
    { NULL }
};

static const gchar *description_text =
    "\n"
    "Examples:\n"
    "  # Run in the foreground with the default config\n"
    "  clawtillad --foreground\n"
    "\n"
    "  # Run against an explicit config\n"
    "  clawtillad -c ~/.clawtilla/config.yaml\n"
    "\n"
    "  # Install as a user service\n"
    "  clawtilla --generate-systemd-service > ~/.config/systemd/user/clawtilla.service\n"
    "  systemctl --user enable --now clawtilla\n"
    "\n"
    "Configuration is read from the path given with -c, otherwise from\n"
    "~/.clawtilla/config.yaml.\n";

/*
 * SIGINT and SIGTERM ask for a clean stop rather than dying where we
 * stand: agents get stopped, transcripts get flushed and the sockets get
 * removed, so the next start does not have to clear up after this one.
 */
static gboolean
on_signal(gpointer user_data)
{
    g_info("clawtillad: shutting down");
    clawt_daemon_quit(CLAWT_DAEMON(user_data));

    return G_SOURCE_REMOVE;
}

/* SIGHUP re-reads the configuration, as a service is expected to. */
static gboolean
on_reload_signal(gpointer user_data)
{
    g_autoptr(GError) error = NULL;

    if (!clawt_daemon_reload(CLAWT_DAEMON(user_data), &error))
        g_warning("clawtillad: reload failed, keeping the old config: %s",
                  error->message);
    else
        g_info("clawtillad: configuration reloaded");

    return G_SOURCE_CONTINUE;
}

int
main(int argc, char *argv[])
{
    g_autoptr(GOptionContext) context = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtDaemon) daemon = NULL;

    context = g_option_context_new("- the clawtilla agent orchestration daemon");
    g_option_context_add_main_entries(context, entries, NULL);
    g_option_context_set_description(context, description_text);

    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        g_printerr("clawtillad: %s\n", error->message);
        return EXIT_FAILURE;
    }

    if (opt_version) {
        g_print("clawtillad %d.%d.%d (%s)\n",
                CLAWT_VERSION_MAJOR, CLAWT_VERSION_MINOR, CLAWT_VERSION_MICRO,
                CLAWT_GIT_SHA);
        return EXIT_SUCCESS;
    }

    daemon = clawt_daemon_new(opt_config_path, NULL);

    /*
     * Signals are handled here rather than in the library, because a host
     * that embeds ClawtDaemon owns its own signal handling and would not
     * thank us for installing ours behind its back.
     */
    g_unix_signal_add(SIGINT, on_signal, daemon);
    g_unix_signal_add(SIGTERM, on_signal, daemon);
    g_unix_signal_add(SIGHUP, on_reload_signal, daemon);

    if (!opt_foreground) {
        /*
         * There is no fork-and-detach here on purpose.  This runs under
         * systemd --user as a Type=simple service, which wants the process
         * in the foreground; daemonising ourselves would make systemd
         * think the service exited immediately.  The flag is accepted so
         * that scripts written against it keep working, and says so.
         */
        g_info("clawtillad: running in the foreground; --foreground is the "
               "only supported mode under systemd");
    }

    return clawt_daemon_run(daemon);
}
