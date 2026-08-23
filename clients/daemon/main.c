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
#include <string.h>

static gboolean opt_foreground = FALSE;
static gboolean opt_version = FALSE;
static gchar   *opt_config_path = NULL;
static gboolean opt_no_bind = FALSE;
static gchar  **opt_bind = NULL;

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
        "bind", 'b', 0, G_OPTION_ARG_STRING_ARRAY, &opt_bind,
        "Listen on this address instead of the configured ones; "
        "repeatable", "IP[:PORT]"
    },
    {
        "no-bind", 0, 0, G_OPTION_ARG_NONE, &opt_no_bind,
        "Do not listen on the network at all; the unix socket only", NULL
    },
    {
        "version", 'V', 0, G_OPTION_ARG_NONE, &opt_version,
        "Print version information and exit", NULL
    },
    { NULL }
};

static const gchar *examples_text =
    "\n"
    "Examples:\n"
    "  # Run in the foreground with the default config\n"
    "  clawtillad --foreground\n"
    "\n"
    "  # Run against an explicit config\n"
    "  clawtillad -c ~/.clawtilla/config.yaml\n"
    "\n"
    "  # Keep the fleet entirely local\n"
    "  clawtillad --no-bind\n"
    "\n"
    "  # Listen somewhere specific instead\n"
    "  clawtillad --bind 127.0.0.1:8792\n"
    "  clawtillad --bind 10.0.0.5:9000 --bind '[fd7a:115c:a1e0::1]:9000'\n"
    "\n"
    "  # Install as a user service\n"
    "  clawtilla --generate-systemd-service > ~/.config/systemd/user/clawtilla.service\n"
    "  systemctl --user enable --now clawtilla\n"
    "\n";

/*
 * The config path, found before the options are parsed.
 *
 * --help is answered by GOption during the parse, so the defaults shown
 * in it have to be worked out before that -- and what they are depends on
 * which config is in play. A small scan of argv rather than a second
 * option pass, because this only needs one answer and getting it wrong
 * costs a line of help text rather than a wrong config being loaded: the
 * real parse still decides what the daemon runs against.
 */
static const gchar *
peek_config_path(int argc, char *argv[])
{
    gint i;

    for (i = 1; i < argc; i++) {
        if (g_strcmp0(argv[i], "--") == 0)
            break;

        if ((g_strcmp0(argv[i], "-c") == 0 ||
             g_strcmp0(argv[i], "--config") == 0) && i + 1 < argc)
            return argv[i + 1];

        if (g_str_has_prefix(argv[i], "--config="))
            return argv[i] + strlen("--config=");
    }

    return NULL;
}

/*
 * What this machine would listen on if nothing on the command line said
 * otherwise -- with the real address and the real port in it.
 *
 * "the tailnet address" in help text is not something a person can check
 * against `ss -ltn`, and the whole reason for --bind and --no-bind is
 * that somebody wanted to know, and change, exactly this.
 */
static gchar *
describe_defaults(int argc, char *argv[])
{
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *tailnet = NULL;
    g_autofree gchar *socket_path = NULL;
    GString *out = g_string_new(examples_text);
    gint64 port = CLAWT_DEFAULT_TCP_PORT;
    gboolean tcp = FALSE;
    gboolean tailscale = TRUE;

    config = clawt_config_load(peek_config_path(argc, argv), &error);

    if (config != NULL) {
        port = clawt_config_get_int(config, "daemon.tcp_port");
        tcp = clawt_config_get_boolean(config, "daemon.tcp_enabled");
        tailscale = clawt_config_get_boolean(config, "daemon.tailscale");
        socket_path = clawt_config_get_path_value(config, "daemon.socket");
    }

    if (tailscale)
        tailnet = clawt_tailscale_find_address();

    g_string_append(out, "Listening, with no --bind:\n\n");

    g_string_append_printf(out, "  unix     %s\n",
                           socket_path != NULL
                               ? socket_path
                               : "$XDG_RUNTIME_DIR/clawtilla/daemon.sock");

    if (!tailscale)
        g_string_append(out,
                        "  tailnet  off (daemon.tailscale: false)\n");
    else if (tailnet != NULL)
        g_string_append_printf(out,
                               "  tailnet  %s:%" G_GINT64_FORMAT
                               "  (daemon.tailscale: true)\n",
                               tailnet, port);
    else
        g_string_append(out,
                        "  tailnet  on, but this machine has no tailnet "
                        "address\n");

    if (tcp) {
        const gchar *address = clawt_config_get_string(config,
                                                        "daemon.tcp_address");

        g_string_append_printf(out, "  tcp      %s:%" G_GINT64_FORMAT
                               "  (daemon.tcp_enabled: true)\n",
                               address != NULL ? address : "127.0.0.1", port);
    } else {
        g_string_append(out, "  tcp      off (daemon.tcp_enabled: false)\n");
    }

    g_string_append(out,
        "\n"
        "--bind replaces all of the network addresses above; it never adds\n"
        "to them, so a daemon told where to listen is not also listening\n"
        "somewhere it was not asked to. The port defaults to "
        "daemon.tcp_port.\n"
        "An address given here must bind: unlike the tailnet address, which\n"
        "clawtilla chose and will warn about, one you named is an error.\n"
        "\n"
        "--no-bind removes them and keeps the unix socket, which is how the\n"
        "daemon is reached from this machine either way.\n"
        "\n"
        "A network listener always requires a token, generated into\n"
        "<state_dir>/tcp-token if daemon.token_file names nothing.\n"
        "`clawtilla daemon token` prints it.\n"
        "\n"
        "Configuration is read from the path given with -c, otherwise from\n"
        "~/.clawtilla/config.yaml.\n");

    return g_string_free(out, FALSE);
}

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
    g_autofree gchar *description = describe_defaults(argc, argv);

    context = g_option_context_new("- the clawtilla agent orchestration daemon");
    g_option_context_add_main_entries(context, entries, NULL);
    g_option_context_set_description(context, description);

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

    /*
     * Contradictory, and silently picking one would mean a daemon
     * listening on an address the person had also just asked it not to.
     */
    if (opt_no_bind && opt_bind != NULL) {
        g_printerr("clawtillad: --no-bind and --bind contradict each "
                   "other\n");
        return EXIT_FAILURE;
    }

    daemon = clawt_daemon_new(opt_config_path, NULL);

    /*
     * Applied before start, and the address is parsed here rather than
     * carried as text: `--bind nonsense` is refused while the person is
     * still looking at the command line, not after the state directory
     * and every agent workspace have been written.
     */
    if (opt_no_bind || opt_bind != NULL) {
        if (!clawt_daemon_set_bind_addresses(
                daemon, (const gchar *const *)opt_bind, &error)) {
            g_printerr("clawtillad: --bind: %s\n", error->message);
            return EXIT_FAILURE;
        }
    }

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
