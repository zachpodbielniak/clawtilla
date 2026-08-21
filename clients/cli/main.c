/*
 * main.c - clawtilla command-line client
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Subcommands are dispatched on argv[1] before GOptionContext ever sees the
 * arguments, and each subcommand owns its own option context.  libreclaw's
 * CLI does the same thing for the same reason: a single shared option table
 * across a dozen verbs collapses into mutually exclusive flags that are only
 * valid for one verb each, and the --help output stops being readable.
 */

#include <clawtilla.h>

/*
 * libreclaw's umbrella header does not pull in its own version macros, so
 * ask for them directly.  Reporting which libreclaw a clawtilla was built
 * against matters: the two version independently and the clawtilla channel
 * only exists from libreclaw 0.26.0 onwards.
 */
#include <lc-version.h>

#include <stdlib.h>

/* Generated from data/default-config.yaml at build time. */
#include "clawt-default-config.h"

static gboolean opt_version = FALSE;
static gboolean opt_license = FALSE;
static gboolean opt_generate_config = FALSE;
static gchar   *opt_config_path = NULL;

static GOptionEntry entries[] = {
    {
        "config", 'c', 0, G_OPTION_ARG_FILENAME, &opt_config_path,
        "Path to the clawtilla configuration YAML", "FILE"
    },
    {
        "version", 'V', 0, G_OPTION_ARG_NONE, &opt_version,
        "Print version information and exit", NULL
    },
    {
        "license", 0, 0, G_OPTION_ARG_NONE, &opt_license,
        "Print licensing information and exit", NULL
    },
    {
        "generate-config", 0, 0, G_OPTION_ARG_NONE, &opt_generate_config,
        "Print a starter config YAML to stdout and exit", NULL
    },
    { NULL }
};

/*
 * The usage block is hand-written rather than left to GOptionContext.  The
 * generated summary lists flags but says nothing about the verbs, and the
 * verbs are the whole interface.
 */
static const gchar *usage_text =
    "clawtilla - orchestrate a fleet of libreclaw agents\n"
    "\n"
    "Usage:\n"
    "  clawtilla [OPTION...]\n"
    "  clawtilla <command> [ARGS...]\n"
    "\n"
    "Commands:\n"
    "  daemon                        Run the daemon in the foreground\n"
    "  agent <verb>                  Manage agents (list, show, create, start, ...)\n"
    "  send <target> <message>       Send a message to an agent or room\n"
    "  chat <agent>                  Interactive chat with one agent\n"
    "  mailbox <verb>                Inspect and manage an agent's mailbox\n"
    "  room <verb>                   Manage rooms\n"
    "  task <verb>                   Inspect delegated tasks\n"
    "  computer <verb>               Run commands on an agent's computer\n"
    "  cp <src> <dst>                Copy files to or from an agent's computer\n"
    "  config <verb>                 Show, validate or render configuration\n"
    "  plugin list                   List loaded plugins\n"
    "\n"
    "Examples:\n"
    "  # Write a starter config and start the daemon\n"
    "  clawtilla --generate-config > ~/.clawtilla/config.yaml\n"
    "  clawtillad --foreground\n"
    "\n"
    "  # Create an agent with its own container, then talk to it\n"
    "  clawtilla agent create --id researcher --model sonnet --computer container\n"
    "  clawtilla agent start researcher\n"
    "  clawtilla chat researcher\n"
    "\n"
    "  # Hand work to a chief-of-staff and watch it delegate\n"
    "  clawtilla send chief-of-staff \"summarize this week's commits\"\n"
    "  clawtilla task list\n"
    "\n"
    "  # Look at what is queued for an agent that is currently stopped\n"
    "  clawtilla mailbox list researcher\n"
    "\n"
    "Configuration is read from the path given with -c, otherwise from\n"
    "~/.clawtilla/config.yaml.\n";

static void
print_version(void)
{
    g_print("clawtilla %d.%d.%d (%s)\n",
            CLAWT_VERSION_MAJOR, CLAWT_VERSION_MINOR, CLAWT_VERSION_MICRO,
            CLAWT_GIT_SHA);
    g_print("libreclaw %d.%d.%d\n",
            LC_VERSION_MAJOR, LC_VERSION_MINOR, LC_VERSION_MICRO);
}

static void
print_license(void)
{
    g_print(
        "clawtilla - orchestrate a fleet of libreclaw agents\n"
        "Copyright (C) 2026\n"
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
        "<https://www.gnu.org/licenses/>.\n");
}

int
main(int argc, char *argv[])
{
    g_autoptr(GOptionContext) context = NULL;
    g_autoptr(GError) error = NULL;

    context = g_option_context_new("- orchestrate a fleet of libreclaw agents");
    g_option_context_add_main_entries(context, entries, NULL);
    g_option_context_set_description(context, usage_text);

    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        g_printerr("clawtilla: %s\n", error->message);
        return EXIT_FAILURE;
    }

    if (opt_version) {
        print_version();
        return EXIT_SUCCESS;
    }

    if (opt_license) {
        print_license();
        return EXIT_SUCCESS;
    }

    if (opt_generate_config) {
        g_print("%s", default_config_yaml);
        return EXIT_SUCCESS;
    }

    /*
     * No verb and no flag: print the usage block rather than doing nothing.
     * Exit non-zero, because being invoked with no arguments is a mistake
     * and a script should be able to notice.
     */
    if (argc < 2) {
        g_printerr("%s", usage_text);
        return EXIT_FAILURE;
    }

    g_printerr("clawtilla: unknown command '%s'\n", argv[1]);
    g_printerr("Run 'clawtilla --help' for usage.\n");
    return EXIT_FAILURE;
}
