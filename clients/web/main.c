/*
 * main.c - clawtilla-web, the HTMX web client
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A ClawtClient talking to the daemon over the same socket protocol every
 * other client uses, rendered server-side with htmx-glib.  This is the one
 * surface that is deliberately incomplete: agent list, transcript and send
 * work; everything else is marked TODO rather than faked.
 */

#include <clawtilla.h>

#include <stdlib.h>

static gint   opt_port = 8790;
static gchar *opt_socket = NULL;
static gboolean opt_version = FALSE;

static GOptionEntry entries[] = {
    {
        "port", 'p', 0, G_OPTION_ARG_INT, &opt_port,
        "Port to listen on (default: 8790)", "PORT"
    },
    {
        "socket", 's', 0, G_OPTION_ARG_FILENAME, &opt_socket,
        "Path to the clawtilla daemon socket", "PATH"
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
    "  # Serve the web client on the default port\n"
    "  clawtilla-web\n"
    "\n"
    "  # Serve on another port against an explicit daemon socket\n"
    "  clawtilla-web --port 9000 --socket /run/user/1000/clawtilla/daemon.sock\n";

int
main(int argc, char *argv[])
{
    g_autoptr(GOptionContext) context = NULL;
    g_autoptr(GError) error = NULL;

    context = g_option_context_new("- the clawtilla web client");
    g_option_context_add_main_entries(context, entries, NULL);
    g_option_context_set_description(context, description_text);

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

    g_printerr("clawtilla-web: not wired up yet\n");
    return EXIT_FAILURE;
}
