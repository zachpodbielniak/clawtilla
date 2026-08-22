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

#include <unistd.h>

/*
 * libreclaw's umbrella header does not pull in its own version macros, so
 * ask for them directly.  Reporting which libreclaw a clawtilla was built
 * against matters: the two version independently and the clawtilla channel
 * only exists from libreclaw 0.26.0 onwards.
 */
#include <lc-version.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/* Generated from data/default-config.yaml at build time. */
#include "clawt-default-config.h"

static gboolean opt_version = FALSE;
static gchar   *opt_socket_path = NULL;
static gboolean opt_license = FALSE;
static gboolean opt_generate_config = FALSE;
static gboolean opt_generate_service = FALSE;
static gchar   *opt_config_path = NULL;

static GOptionEntry entries[] = {
    {
        "config", 'c', 0, G_OPTION_ARG_FILENAME, &opt_config_path,
        "Path to the clawtilla configuration YAML", "FILE"
    },
    {
        "socket", 's', 0, G_OPTION_ARG_FILENAME, &opt_socket_path,
        "Path to the daemon's socket", "FILE"
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
    {
        "generate-systemd-service", 0, 0, G_OPTION_ARG_NONE,
        &opt_generate_service,
        "Print a systemd --user unit to stdout and exit", NULL
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
    "  image                         Container images clawtilla suggests\n"
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
    "  # Edit who it is: the org files that become its system prompt\n"
    "  clawtilla agent files researcher\n"
    "  clawtilla agent edit researcher            # all of them, in order\n"
    "  clawtilla agent edit researcher TOOLS.org  # just one\n"
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

/*
 * Every verb, in one place.
 *
 * Used to work out where the global options stop and the subcommand
 * begins, so it has to list exactly what main() dispatches on -- a verb
 * missing here would have its arguments handed to the global parser.
 */
static const gchar *const verbs[] = {
    "daemon", "status", "agent", "send", "chat", "mailbox", "room", "task",
    "computer", "cp", "config", "plugin", "integration", "model", "image",
    NULL
};

/*
 * Returns: the index of the first verb in @argv, or 0 if there is none
 */
static gint
find_verb(gint argc, gchar **argv)
{
    gint i;

    for (i = 1; i < argc; i++) {
        gsize j;

        for (j = 0; verbs[j] != NULL; j++) {
            if (g_strcmp0(argv[i], verbs[j]) == 0)
                return i;
        }
    }

    return 0;
}

static void
print_version(void)
{
    g_print("clawtilla %d.%d.%d (%s)\n",
            CLAWT_VERSION_MAJOR, CLAWT_VERSION_MINOR, CLAWT_VERSION_MICRO,
            CLAWT_GIT_SHA);
    g_print("libreclaw %d.%d.%d\n",
            LC_VERSION_MAJOR, LC_VERSION_MINOR, LC_VERSION_MICRO);
}

/*
 * The unit names the binary by the path this build was configured with,
 * so a locally-built clawtilla does not silently generate a unit pointing
 * at a packaged one somewhere else on the system.
 */
static void
print_systemd_service(void)
{
    g_print(
        "# clawtilla.service - the clawtilla agent orchestration daemon\n"
        "#\n"
        "# Install with:\n"
        "#   clawtilla --generate-systemd-service \\\n"
        "#     > ~/.config/systemd/user/clawtilla.service\n"
        "#   systemctl --user daemon-reload\n"
        "#   systemctl --user enable --now clawtilla\n"
        "\n"
        "[Unit]\n"
        "Description=clawtilla agent orchestration daemon\n"
        "After=network.target\n"
        "\n"
        "[Service]\n"
        "Type=simple\n"
        "ExecStart=%s/clawtillad --foreground "
        "--config %%h/.clawtilla/config.yaml\n"
        "ExecReload=/bin/kill -HUP $MAINPID\n"
        "\n"
        "# Restarted on failure but not on a clean exit, so asking the\n"
        "# daemon to shut down actually shuts it down.\n"
        "Restart=on-failure\n"
        "RestartSec=5\n"
        "KillMode=mixed\n"
        "TimeoutStopSec=30\n"
        "\n"
        "[Install]\n"
        "WantedBy=default.target\n"
        "\n"
        "# The agents' sockets live under XDG_RUNTIME_DIR, which systemd\n"
        "# removes when your last session ends.  For a fleet that survives\n"
        "# logout:  loginctl enable-linger $USER\n",
        CLAWT_BINDIR);
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


/* ── Talking to the daemon ───────────────────────────────────────── */

/*
 * Every verb goes through the daemon rather than touching the config or
 * the agents directly.
 *
 * That is the rule the whole design rests on: the daemon owns every
 * transport, every credential and every agent process.  A CLI that edited
 * the config file itself would race the daemon that is reading it, and a
 * CLI that started an agent itself would produce a second fleet nobody is
 * managing.
 */
static ClawtClient *
connect_to_daemon(void)
{
    ClawtClient *client;
    g_autoptr(GError) error = NULL;

    client = clawt_client_new(opt_socket_path);

    if (!clawt_client_connect(client, &error)) {
        g_printerr("clawtilla: %s\n", error->message);
        g_object_unref(client);
        return NULL;
    }

    return client;
}

static JsonNode *
build_payload(const gchar *first_key, ...)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    const gchar *key = first_key;
    va_list args;

    json_builder_begin_object(builder);

    va_start(args, first_key);

    while (key != NULL) {
        const gchar *value = va_arg(args, const gchar *);

        if (value != NULL) {
            json_builder_set_member_name(builder, key);
            json_builder_add_string_value(builder, value);
        }

        key = va_arg(args, const gchar *);
    }

    va_end(args);

    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

static JsonNode *
call(ClawtClient *client, const gchar *kind, JsonNode *payload)
{
    g_autoptr(GError) error = NULL;
    JsonNode *reply;

    reply = clawt_client_request(client, kind, payload, &error);

    if (reply == NULL)
        g_printerr("clawtilla: %s\n", error->message);

    return reply;
}

static const gchar *
member_or(JsonObject *object, const gchar *key, const gchar *fallback)
{
    if (object == NULL || !json_object_has_member(object, key))
        return fallback;

    if (json_node_get_value_type(json_object_get_member(object, key)) !=
        G_TYPE_STRING)
        return fallback;

    return json_object_get_string_member(object, key);
}

/* ── agent ───────────────────────────────────────────────────────── */

/*
 * The editor to open workspace files with.
 *
 * VISUAL first, then EDITOR, then a fall-through of what is actually
 * installed -- the same order a shell profile uses, so a user who has
 * set one gets it and a user who has set neither still gets an editor
 * rather than an error.
 */
static gchar *
resolve_editor(void)
{
    static const gchar *const candidates[] = {
        "nvim", "vim", "vi", "emacsclient_tty", "emacs", "nano", NULL
    };
    const gchar *configured;
    gsize i;

    configured = g_getenv("VISUAL");

    if (configured == NULL || *configured == '\0')
        configured = g_getenv("EDITOR");

    if (configured != NULL && *configured != '\0')
        return g_strdup(configured);

    for (i = 0; candidates[i] != NULL; i++) {
        g_autofree gchar *found = g_find_program_in_path(candidates[i]);

        if (found != NULL)
            return g_steal_pointer(&found);
    }

    return NULL;
}

/*
 * Runs $EDITOR on the given paths and waits.
 *
 * $EDITOR is a command line, not a program name -- "emacsclient -nw" and
 * "code --wait" are both normal -- so it is split with shell quoting
 * rules rather than exec'd whole.
 *
 * Inherits this process's stdin/stdout: the editor is a terminal program
 * and the whole point is that the user drives it.
 */
static gint
run_editor(GPtrArray *paths)
{
    g_autofree gchar *editor = resolve_editor();
    g_autoptr(GPtrArray) argv = NULL;
    g_auto(GStrv) parts = NULL;
    g_autoptr(GError) error = NULL;
    gint status = 0;
    guint i;

    if (editor == NULL) {
        g_printerr("clawtilla: no editor found; set $EDITOR or $VISUAL\n");
        return EXIT_FAILURE;
    }

    if (!g_shell_parse_argv(editor, NULL, &parts, &error)) {
        g_printerr("clawtilla: $EDITOR is not a runnable command line "
                   "(%s): %s\n", editor, error->message);
        return EXIT_FAILURE;
    }

    argv = g_ptr_array_new();

    for (i = 0; parts[i] != NULL; i++)
        g_ptr_array_add(argv, parts[i]);

    for (i = 0; i < paths->len; i++)
        g_ptr_array_add(argv, g_ptr_array_index(paths, i));

    g_ptr_array_add(argv, NULL);

    if (!g_spawn_sync(NULL, (gchar **)argv->pdata, NULL,
                      G_SPAWN_SEARCH_PATH | G_SPAWN_CHILD_INHERITS_STDIN,
                      NULL, NULL, NULL, NULL, &status, &error)) {
        g_printerr("clawtilla: could not run %s: %s\n", editor,
                   error->message);
        return EXIT_FAILURE;
    }

    /*
     * The editor's exit status is this process's.  An editor that failed
     * to save is not a successful edit, and a script doing
     * `clawtilla agent edit ... && clawtilla agent restart ...` needs to
     * be able to tell.
     */
    if (!g_spawn_check_wait_status(status, &error)) {
        g_printerr("clawtilla: %s: %s\n", editor, error->message);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

static gint
cmd_agent(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    const gchar *verb = (argc > 2) ? argv[2] : NULL;
    const gchar *target = (argc > 3) ? argv[3] : NULL;

    if (verb == NULL) {
        g_printerr("Usage: clawtilla agent "
                   "<list|show|create|rm|start|stop|restart|logs|set|"
                   "files|edit|mount> "
                   "[ARGS...]\n");
        return EXIT_FAILURE;
    }

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    if (g_strcmp0(verb, "list") == 0) {
        JsonArray *agents;
        guint i;

        reply = call(client, "agent.list", NULL);
        if (reply == NULL)
            return EXIT_FAILURE;

        agents = json_object_get_array_member(json_node_get_object(reply),
                                              "agents");

        if (json_array_get_length(agents) == 0) {
            g_print("No agents yet. Create one with:\n"
                    "  clawtilla agent create --id chief-of-staff\n");
            return EXIT_SUCCESS;
        }

        g_print("%-20s %-10s %-6s %-8s %s\n", "ID", "STATE", "QUEUE",
                "LINK", "DESCRIPTION");

        for (i = 0; i < json_array_get_length(agents); i++) {
            JsonObject *agent = json_array_get_object_element(agents, i);

            g_print("%-20s %-10s %-6" G_GINT64_FORMAT " %-8s %s\n",
                    member_or(agent, "id", "?"),
                    member_or(agent, "state", "?"),
                    json_object_get_int_member(agent, "mailbox_depth"),
                    json_object_get_boolean_member(agent, "connected")
                        ? "up" : "-",
                    member_or(agent, "description", ""));
        }

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "show") == 0) {
        JsonObject *agent;

        if (target == NULL) {
            g_printerr("Usage: clawtilla agent show <agent>\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "agent.show",
                     build_payload("agent", target, NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        agent = json_object_get_object_member(json_node_get_object(reply),
                                              "agent");

        g_print("id:          %s\n", member_or(agent, "id", "?"));
        g_print("name:        %s\n", member_or(agent, "name", "?"));
        g_print("description: %s\n", member_or(agent, "description", "-"));
        g_print("state:       %s%s%s\n", member_or(agent, "state", "?"),
                json_object_has_member(agent, "detail") ? " - " : "",
                member_or(agent, "detail", ""));
        g_print("model:       %s\n", member_or(agent, "model", "-"));

        /* Only container agents have one, so only they report one. */
        if (json_object_has_member(agent, "image"))
            g_print("image:       %s\n", member_or(agent, "image", "-"));

        /* Likewise the VM's size, which only a VM has. */
        if (json_object_has_member(agent, "vm_cpus"))
            g_print("vm:          %s core(s), %s MB\n",
                    member_or(agent, "vm_cpus", "?"),
                    member_or(agent, "vm_memory_mb", "?"));
        g_print("computer:    %s\n", member_or(agent, "computer", "none"));
        g_print("can:         %s\n", member_or(agent, "caps", "-"));
        g_print("queue:       %" G_GINT64_FORMAT "\n",
                json_object_get_int_member(agent, "mailbox_depth"));

        if (json_object_has_member(agent, "credentials")) {
            JsonObject *credentials =
                json_object_get_object_member(agent, "credentials");
            g_autoptr(GList) names = json_object_get_members(credentials);
            GList *l;

            for (l = names; l != NULL; l = l->next)
                g_print("credential:  %s -> %s\n", (const gchar *)l->data,
                        json_object_get_string_member(credentials, l->data));
        }

        if (json_object_has_member(json_node_get_object(reply),
                                   "computer_detail"))
            g_print("\n%s\n",
                    json_object_get_string_member(json_node_get_object(reply),
                                                  "computer_detail"));

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "new") == 0) {
        /*
         * The questionnaire.
         *
         * One free-text description asked the person to write a
         * paragraph that happened to contain everything the model
         * needed, and a paragraph that leaves out the boundaries
         * produces an agent with none.  Each question asks for one thing
         * once; only the first is required.
         */
        static const struct {
            const gchar *field;
            const gchar *prompt;
            const gchar *hint;
        } questions[] = {
            { "purpose", "What should this agent do?",
              "e.g. reads my notes and answers questions about them" },
            { "boundaries", "What should it never do?",
              "e.g. never push to main, never touch production" },
            { "needs", "What does it need to work on?",
              "files, commands, the network, or nothing" },
            { "personality", "How should it come across?",
              "e.g. terse and blunt; no preamble" },
            { "projects", "What is it working on, and where?",
              "e.g. ~/source/projects/foo" },
            { "notes", "Anything else it should know?", NULL },
            { NULL, NULL, NULL }
        };
        g_autoptr(GString) description = g_string_new(NULL);
        g_autoptr(JsonBuilder) builder = json_builder_new();
        g_autoptr(GHashTable) answers =
            g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
        const gchar *designer_provider = NULL;
        const gchar *designer_model = NULL;
        gboolean use_ai = FALSE;
        gboolean commit = FALSE;
        gint i;
        gsize q;

        for (i = 3; i < argc; i++) {
            if (g_strcmp0(argv[i], "--ai") == 0) {
                use_ai = TRUE;
                continue;
            }

            if (g_strcmp0(argv[i], "--commit") == 0) {
                commit = TRUE;
                continue;
            }

            if (g_strcmp0(argv[i], "--designer") == 0 && i + 1 < argc) {
                designer_provider = argv[++i];
                continue;
            }

            if (g_strcmp0(argv[i], "--designer-model") == 0 && i + 1 < argc) {
                designer_model = argv[++i];
                continue;
            }

            if (description->len > 0)
                g_string_append_c(description, ' ');

            g_string_append(description, argv[i]);
        }

        /*
         * No description means ask.  A person who runs the bare verb is
         * asking to be walked through it, and an interactive prompt is
         * how every other tool on their machine handles that.
         */
        if (description->len == 0 && isatty(STDIN_FILENO)) {
            g_print("Designing an agent. Answer what you can; press Enter to "
                    "skip.\n");
            g_print("A skipped answer becomes a heading to fill in, not an "
                    "invention.\n\n");

            for (q = 0; questions[q].field != NULL; q++) {
                gchar line[1024];

                g_print("%s\n", questions[q].prompt);

                if (questions[q].hint != NULL)
                    g_print("  (%s)\n", questions[q].hint);

                g_print("> ");

                if (fgets(line, sizeof(line), stdin) == NULL)
                    break;

                g_strstrip(line);
                g_print("\n");

                if (*line != '\0')
                    g_hash_table_insert(answers, (gpointer)questions[q].field,
                                        g_strdup(line));
            }

            if (g_hash_table_lookup(answers, "purpose") == NULL) {
                g_printerr("clawtilla: nothing to design; say what the agent "
                           "should do.\n");
                return EXIT_FAILURE;
            }

            use_ai = TRUE;
        } else if (!use_ai) {
            g_printerr("Usage: clawtilla agent new            "
                       "# ask me the questions\n");
            g_printerr("       clawtilla agent new --ai <description>\n");
            g_printerr("  For a hand-written agent use "
                       "'clawtilla agent create --id <id>'.\n");
            return EXIT_FAILURE;
        }

        if (description->len == 0 && g_hash_table_size(answers) == 0) {
            g_printerr("Say what the agent should do, e.g.\n");
            g_printerr("  clawtilla agent new --ai \"reads my notes and "
                       "answers questions about them\"\n");
            return EXIT_FAILURE;
        }

        g_print("Designing...\n");

        json_builder_begin_object(builder);

        for (q = 0; questions[q].field != NULL; q++) {
            const gchar *answer = g_hash_table_lookup(answers,
                                                       questions[q].field);

            if (answer == NULL)
                continue;

            json_builder_set_member_name(builder, questions[q].field);
            json_builder_add_string_value(builder, answer);
        }

        if (description->len > 0) {
            json_builder_set_member_name(builder, "description");
            json_builder_add_string_value(builder, description->str);
        }

        if (designer_provider != NULL) {
            json_builder_set_member_name(builder, "provider");
            json_builder_add_string_value(builder, designer_provider);
        }

        if (designer_model != NULL) {
            json_builder_set_member_name(builder, "model");
            json_builder_add_string_value(builder, designer_model);
        }

        json_builder_end_object(builder);

        reply = call(client, "design.agent", json_builder_get_root(builder));
        if (reply == NULL)
            return EXIT_FAILURE;

        {
            JsonObject *result = json_node_get_object(reply);
            const gchar *draft = member_or(result, "draft", NULL);
            const gchar *id = member_or(result, "id", "?");
            JsonArray *files = json_object_has_member(result, "files")
                               ? json_object_get_array_member(result, "files")
                               : NULL;

            g_print("\n%s\n", member_or(result, "yaml", ""));

            if (files != NULL && json_array_get_length(files) > 0) {
                guint f;

                g_print("It also wrote:\n");

                for (f = 0; f < json_array_get_length(files); f++) {
                    JsonObject *file = json_array_get_object_element(files, f);
                    const gchar *content = member_or(file, "content", "");
                    const gchar *p;
                    gsize lines = 0;

                    for (p = content; *p != '\0'; p++) {
                        if (*p == '\n')
                            lines++;
                    }

                    g_print("  %-18s %" G_GSIZE_FORMAT " lines\n",
                            member_or(file, "name", "?"), lines);
                }

                g_print("\n");
            }

            if (member_or(result, "notes", NULL) != NULL)
                g_print("%s\n\n", json_object_get_string_member(result,
                                                                "notes"));

            /*
             * Asked rather than assumed, and asked here rather than by
             * re-running the design: the draft on the daemon is the one
             * just shown, and committing it creates exactly that.
             */
            if (!commit && isatty(STDIN_FILENO)) {
                gchar line[16];

                g_print("Create it? [y/N] ");

                if (fgets(line, sizeof(line), stdin) != NULL) {
                    g_strstrip(line);
                    commit = (g_ascii_strcasecmp(line, "y") == 0 ||
                              g_ascii_strcasecmp(line, "yes") == 0);
                }
            }

            if (!commit || draft == NULL) {
                g_autoptr(JsonNode) discarded = NULL;

                if (draft != NULL)
                    discarded = call(client, "design.discard",
                                     build_payload("draft", draft, NULL));

                g_print("Nothing has been added.\n");
                return EXIT_SUCCESS;
            }

            {
                g_autoptr(JsonNode) created =
                    call(client, "design.commit",
                         build_payload("draft", draft, NULL));

                if (created == NULL)
                    return EXIT_FAILURE;

                id = member_or(json_node_get_object(created), "id", id);
            }

            g_print("Created %s.\n", id);
            g_print("Read what it thinks it is: clawtilla agent edit %s\n",
                    id);
            g_print("Start it with: clawtilla agent start %s\n", id);
        }

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "create") == 0) {
        g_autoptr(JsonBuilder) builder = json_builder_new();
        gint i;

        json_builder_begin_object(builder);

        for (i = 3; i < argc; i++) {
            const gchar *flag = argv[i];

            if (!g_str_has_prefix(flag, "--") || i + 1 >= argc) {
                g_printerr("clawtilla: unexpected argument '%s'\n", flag);
                g_printerr("Usage: clawtilla agent create --id <id> "
                           "[--name X] [--model X] [--computer X] "
                           "[--image X]\n");
                return EXIT_FAILURE;
            }

            json_builder_set_member_name(builder, flag + 2);
            json_builder_add_string_value(builder, argv[++i]);
        }

        json_builder_end_object(builder);

        reply = call(client, "agent.create", json_builder_get_root(builder));
        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("Created %s.\n",
                member_or(json_node_get_object(reply), "id", "?"));
        g_print("Start it with: clawtilla agent start %s\n",
                member_or(json_node_get_object(reply), "id", "?"));

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "rm") == 0) {
        gboolean with_computer = FALSE;
        gint i;

        if (target == NULL) {
            g_printerr("Usage: clawtilla agent rm <agent> "
                       "[--with-computer]\n");
            return EXIT_FAILURE;
        }

        for (i = 4; i < argc; i++) {
            if (g_strcmp0(argv[i], "--with-computer") == 0)
                with_computer = TRUE;
        }

        reply = call(client, "agent.remove",
                     build_payload("agent", target,
                                   "remove_computer",
                                   with_computer ? "true" : NULL, NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        {
            const gchar *computer = member_or(json_node_get_object(reply),
                                              "computer", NULL);

            /*
             * The computer's fate is reported rather than assumed. A
             * teardown that failed does not fail the removal, and saying
             * "removed" over a container still running would be a lie.
             */
            if (computer != NULL && g_strcmp0(computer, "removed") == 0)
                g_print("%s and its computer are gone.\n", target);
            else if (computer != NULL)
                g_print("%s is gone. Its computer was not removed: %s\n",
                        target, computer);
            else
                g_print("%s: rm\n", target);
        }

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "start") == 0 || g_strcmp0(verb, "stop") == 0 ||
        g_strcmp0(verb, "restart") == 0) {
        g_autofree gchar *kind = NULL;

        if (target == NULL) {
            g_printerr("Usage: clawtilla agent %s <agent>\n", verb);
            return EXIT_FAILURE;
        }

        kind = g_strdup_printf("agent.%s",
                               g_strcmp0(verb, "rm") == 0 ? "remove" : verb);

        reply = call(client, kind, build_payload("agent", target, NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("%s: %s\n", target, verb);
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "logs") == 0) {
        JsonArray *lines;
        guint i;

        if (target == NULL) {
            g_printerr("Usage: clawtilla agent logs <agent>\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "agent.logs",
                     build_payload("agent", target, NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        lines = json_object_get_array_member(json_node_get_object(reply),
                                             "lines");

        for (i = 0; i < json_array_get_length(lines); i++)
            g_print("%s\n", json_array_get_string_element(lines, i));

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "mount") == 0) {
        const gchar *action = (argc > 3) ? argv[3] : NULL;
        const gchar *who = (argc > 4) ? argv[4] : NULL;

        if (action == NULL || who == NULL) {
            g_printerr("Usage: clawtilla agent mount list <agent>\n");
            g_printerr("       clawtilla agent mount add <agent> "
                       "<source> <target> [ro|rw]\n");
            g_printerr("       clawtilla agent mount rm <agent> <target>\n");
            g_printerr("  e.g. clawtilla agent mount add researcher "
                       "~/src /work rw\n");
            return EXIT_FAILURE;
        }

        if (g_strcmp0(action, "list") == 0) {
            JsonArray *mounts;
            guint i;

            reply = call(client, "agent.mount.list",
                         build_payload("agent", who, NULL));
            if (reply == NULL)
                return EXIT_FAILURE;

            mounts = json_object_get_array_member(json_node_get_object(reply),
                                                   "mounts");

            if (json_array_get_length(mounts) == 0) {
                g_print("%s shares no folders.\n", who);
                g_print("Add one with: clawtilla agent mount add %s "
                        "<source> <target>\n", who);
                return EXIT_SUCCESS;
            }

            for (i = 0; i < json_array_get_length(mounts); i++) {
                JsonObject *mount = json_array_get_object_element(mounts, i);

                g_print("%-34s %-24s %-4s %s\n",
                        member_or(mount, "source", "-"),
                        member_or(mount, "target", "?"),
                        member_or(mount, "mode", "ro"),
                        member_or(mount, "type", "bind"));
            }

            return EXIT_SUCCESS;
        }

        if (g_strcmp0(action, "rm") == 0) {
            if (argc < 6) {
                g_printerr("Usage: clawtilla agent mount rm <agent> "
                           "<target>\n");
                return EXIT_FAILURE;
            }

            reply = call(client, "agent.mount.remove",
                         build_payload("agent", who, "target", argv[5],
                                       NULL));
            if (reply == NULL)
                return EXIT_FAILURE;

            g_print("%s no longer shares %s.\n", who, argv[5]);
            g_print("Takes effect when the agent next starts.\n");
            return EXIT_SUCCESS;
        }

        if (g_strcmp0(action, "add") == 0) {
            if (argc < 7) {
                g_printerr("Usage: clawtilla agent mount add <agent> "
                           "<source> <target> [ro|rw]\n");
                return EXIT_FAILURE;
            }

            reply = call(client, "agent.mount.add",
                         build_payload("agent", who,
                                       "source", argv[5],
                                       "target", argv[6],
                                       "mode", (argc > 7) ? argv[7] : "ro",
                                       NULL));
            if (reply == NULL)
                return EXIT_FAILURE;

            g_print("%s now sees %s at %s.\n", who, argv[5], argv[6]);
            g_print("Takes effect when the agent next starts.\n");
            return EXIT_SUCCESS;
        }

        g_printerr("clawtilla: unknown mount action '%s'\n", action);
        return EXIT_FAILURE;
    }

    if (g_strcmp0(verb, "files") == 0 || g_strcmp0(verb, "edit") == 0) {
        g_autoptr(GPtrArray) paths = NULL;
        JsonArray *files;
        guint i;

        if (target == NULL) {
            g_printerr("Usage: clawtilla agent %s <agent> [FILE...]\n", verb);
            g_printerr("  e.g. clawtilla agent edit researcher TOOLS.org\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "agent.files",
                     build_payload("agent", target, NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        files = json_object_get_array_member(json_node_get_object(reply),
                                             "files");

        if (g_strcmp0(verb, "files") == 0) {
            g_print("%s\n\n",
                    json_object_get_string_member(json_node_get_object(reply),
                                                  "workspace"));

            for (i = 0; i < json_array_get_length(files); i++) {
                JsonObject *file = json_array_get_object_element(files, i);

                /*
                 * The prompt marker is the useful column: it is the
                 * difference between a file the agent reads every turn
                 * and one that only a person ever opens.
                 */
                g_print("%-18s %s  %s\n",
                        json_object_get_string_member(file, "name"),
                        json_object_get_boolean_member(file, "identity")
                            ? "prompt" : "      ",
                        json_object_get_string_member(file, "title"));
            }

            return EXIT_SUCCESS;
        }

        paths = g_ptr_array_new_with_free_func(g_free);

        if (argc > 4) {
            gint arg;

            /*
             * Named files are matched against the set rather than joined
             * to the workspace here: the daemon owns where a workspace
             * is, and a client building the path itself is a client that
             * can be pointed at somebody else's.
             */
            for (arg = 4; arg < argc; arg++) {
                gboolean found = FALSE;

                for (i = 0; i < json_array_get_length(files); i++) {
                    JsonObject *file = json_array_get_object_element(files, i);

                    if (g_strcmp0(json_object_get_string_member(file, "name"),
                                  argv[arg]) == 0) {
                        g_ptr_array_add(paths, g_strdup(
                            json_object_get_string_member(file, "path")));
                        found = TRUE;
                        break;
                    }
                }

                if (!found) {
                    g_printerr("clawtilla: %s has no file called '%s'; "
                               "`clawtilla agent files %s` lists them\n",
                               target, argv[arg], target);
                    return EXIT_FAILURE;
                }
            }
        } else {
            /*
             * No file named: everything that goes into the prompt, in
             * the order the agent reads it.  The loaders are left out --
             * they are generated, and editing them is how the list stops
             * matching the files.
             */
            for (i = 0; i < json_array_get_length(files); i++) {
                JsonObject *file = json_array_get_object_element(files, i);

                if (json_object_get_boolean_member(file, "identity"))
                    g_ptr_array_add(paths, g_strdup(
                        json_object_get_string_member(file, "path")));
            }
        }

        if (paths->len == 0) {
            g_printerr("clawtilla: nothing to edit\n");
            return EXIT_FAILURE;
        }

        return run_editor(paths);
    }

    if (g_strcmp0(verb, "set") == 0) {
        if (argc < 6) {
            g_printerr("Usage: clawtilla agent set <agent> <key> <value>\n");
            g_printerr("  e.g. clawtilla agent set researcher model.model "
                       "opus\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "agent.set",
                     build_payload("agent", argv[3], "key", argv[4],
                                   "value", argv[5], NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("%s: %s = %s\n", argv[3], argv[4], argv[5]);
        return EXIT_SUCCESS;
    }

    g_printerr("clawtilla: unknown agent verb '%s'\n", verb);
    return EXIT_FAILURE;
}

/* ── send and chat ───────────────────────────────────────────────── */

static gint
cmd_send(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *body = NULL;

    if (argc < 4) {
        g_printerr("Usage: clawtilla send <agent-or-room> <message>\n");
        return EXIT_FAILURE;
    }

    /* Everything after the target, so quoting the message is optional. */
    body = g_strjoinv(" ", argv + 3);

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    reply = call(client, "msg.send",
                 build_payload("target", argv[2], "body", body, NULL));

    if (reply == NULL)
        return EXIT_FAILURE;

    {
        JsonObject *payload = json_node_get_object(reply);
        gint64 queued = json_object_get_int_member(payload, "queued");
        const gchar *state =
            json_object_has_member(payload, "target_state")
            ? json_object_get_string_member(payload, "target_state") : NULL;

        /*
         * Says where it went, because "sent" is ambiguous when the
         * recipient is stopped -- the message is queued and will arrive,
         * and a person watching for a reply should know that.
         */
        if (queued == 0)
            g_print("Nobody was queued: check the room's members.\n");
        else if (state != NULL && g_strcmp0(state, "running") != 0)
            g_print("Queued: %s is %s, so it is held in the mailbox until "
                    "the agent starts.\n", argv[2], state);
        else
            g_print("Queued for %" G_GINT64_FORMAT " recipient(s).\n",
                    queued);
    }

    return EXIT_SUCCESS;
}

static void
on_chat_event(ClawtClient *client, ClawtEvent *event, gpointer user_data)
{
    const gchar *agent_id = user_data;

    (void)client;

    if (g_strcmp0(clawt_event_get_kind(event), "message") != 0)
        return;

    if (g_strcmp0(clawt_event_get_detail(event, "from"), agent_id) != 0)
        return;

    g_print("\n%s: %s\n> ", agent_id,
            clawt_event_get_detail(event, "body"));
}

static gint
cmd_chat(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(GMainContext) context = NULL;
    const gchar *agent_id = (argc > 2) ? argv[2] : NULL;
    gchar line[4096];

    if (agent_id == NULL) {
        g_printerr("Usage: clawtilla chat <agent>\n");
        return EXIT_FAILURE;
    }

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    g_signal_connect(client, "event", G_CALLBACK(on_chat_event),
                     (gpointer)agent_id);

    if (!clawt_client_subscribe(client, 0, NULL, NULL)) {
        g_printerr("clawtilla: could not subscribe to the event stream\n");
        return EXIT_FAILURE;
    }

    g_print("Chatting with %s. Ctrl-D to leave.\n> ", agent_id);

    context = g_main_context_ref(g_main_context_default());

    while (fgets(line, sizeof(line), stdin) != NULL) {
        g_autoptr(JsonNode) reply = NULL;

        g_strstrip(line);

        if (*line == '\0') {
            g_print("> ");
            continue;
        }

        reply = call(client, "msg.send",
                     build_payload("target", agent_id, "body", line, NULL));

        if (reply == NULL)
            return EXIT_FAILURE;

        /*
         * Pumped between turns so replies print while the prompt is
         * waiting.  fgets() blocks, so an event that arrives mid-line is
         * shown as soon as the line is finished rather than lost.
         */
        while (g_main_context_pending(context))
            g_main_context_iteration(context, FALSE);

        g_print("> ");
    }

    g_print("\n");
    return EXIT_SUCCESS;
}

/* ── mailbox ─────────────────────────────────────────────────────── */

static gint
cmd_mailbox(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    const gchar *verb = (argc > 2) ? argv[2] : NULL;
    const gchar *agent_id = (argc > 3) ? argv[3] : NULL;

    if (verb == NULL || agent_id == NULL) {
        g_printerr("Usage: clawtilla mailbox "
                   "<list|dead|ack|requeue|purge> <agent> [item]\n");
        return EXIT_FAILURE;
    }

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    if (g_strcmp0(verb, "list") == 0 || g_strcmp0(verb, "dead") == 0) {
        g_autofree gchar *kind = g_strdup_printf("mailbox.%s", verb);
        JsonArray *items;
        guint i;

        reply = call(client, kind, build_payload("agent", agent_id, NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        items = json_object_get_array_member(json_node_get_object(reply),
                                             "items");

        if (json_array_get_length(items) == 0) {
            g_print("%s's mailbox is empty.\n", agent_id);
            return EXIT_SUCCESS;
        }

        for (i = 0; i < json_array_get_length(items); i++) {
            JsonObject *item = json_array_get_object_element(items, i);

            g_print("%s  %-12s %-8s %s\n", member_or(item, "id", "?"),
                    member_or(item, "from", "?"),
                    member_or(item, "priority", "normal"),
                    member_or(item, "body", ""));

            if (json_object_has_member(item, "last_error"))
                g_print("    last error: %s\n",
                        json_object_get_string_member(item, "last_error"));
        }

        g_print("\n%" G_GINT64_FORMAT " waiting.\n",
                json_object_get_int_member(json_node_get_object(reply),
                                           "depth"));

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "ack") == 0 || g_strcmp0(verb, "requeue") == 0) {
        g_autofree gchar *kind = g_strdup_printf("mailbox.%s", verb);

        if (argc < 5) {
            g_printerr("Usage: clawtilla mailbox %s <agent> <item-id>\n",
                       verb);
            return EXIT_FAILURE;
        }

        reply = call(client, kind,
                     build_payload("agent", agent_id, "item", argv[4], NULL));

        return (reply != NULL) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (g_strcmp0(verb, "purge") == 0) {
        reply = call(client, "mailbox.purge",
                     build_payload("agent", agent_id, NULL));

        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("Purged %" G_GINT64_FORMAT " expired item(s).\n",
                json_object_get_int_member(json_node_get_object(reply),
                                           "purged"));

        return EXIT_SUCCESS;
    }

    g_printerr("clawtilla: unknown mailbox verb '%s'\n", verb);
    return EXIT_FAILURE;
}

/* ── rooms ───────────────────────────────────────────────────────── */

static gint
cmd_room(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    const gchar *verb = (argc > 2) ? argv[2] : NULL;

    if (verb == NULL) {
        g_printerr("Usage: clawtilla room "
                   "<list|create|add|history> [ARGS...]\n");
        return EXIT_FAILURE;
    }

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    if (g_strcmp0(verb, "list") == 0) {
        JsonArray *rooms;
        guint i;

        reply = call(client, "room.list", NULL);
        if (reply == NULL)
            return EXIT_FAILURE;

        rooms = json_object_get_array_member(json_node_get_object(reply),
                                             "rooms");

        for (i = 0; i < json_array_get_length(rooms); i++) {
            JsonObject *room = json_array_get_object_element(rooms, i);
            JsonArray *members = json_object_get_array_member(room,
                                                              "members");
            g_autoptr(GString) list = g_string_new(NULL);
            guint j;

            for (j = 0; j < json_array_get_length(members); j++) {
                if (j > 0)
                    g_string_append(list, ", ");

                g_string_append(list,
                                json_array_get_string_element(members, j));
            }

            g_print("%-24s %s\n", member_or(room, "id", "?"), list->str);
        }

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "create") == 0) {
        const gchar *members = NULL;
        gint i;

        if (argc < 4) {
            g_printerr("Usage: clawtilla room create <room> "
                       "[--members a,b]\n");
            return EXIT_FAILURE;
        }

        for (i = 4; i + 1 < argc; i++) {
            if (g_strcmp0(argv[i], "--members") == 0)
                members = argv[i + 1];
        }

        reply = call(client, "room.create",
                     build_payload("room", argv[3], "members", members,
                                   NULL));

        return (reply != NULL) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (g_strcmp0(verb, "add") == 0) {
        if (argc < 5) {
            g_printerr("Usage: clawtilla room add <room> <agent>\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "room.add",
                     build_payload("room", argv[3], "agent", argv[4], NULL));

        return (reply != NULL) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (g_strcmp0(verb, "history") == 0) {
        JsonArray *messages;
        guint i;

        if (argc < 4) {
            g_printerr("Usage: clawtilla room history <room>\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "room.history",
                     build_payload("room", argv[3], NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        messages = json_object_get_array_member(json_node_get_object(reply),
                                                "messages");

        for (i = 0; i < json_array_get_length(messages); i++) {
            JsonObject *message = json_array_get_object_element(messages, i);

            g_print("%-16s %s\n", member_or(message, "sender", "?"),
                    member_or(message, "body", ""));
        }

        return EXIT_SUCCESS;
    }

    g_printerr("clawtilla: unknown room verb '%s'\n", verb);
    return EXIT_FAILURE;
}

/* ── tasks ───────────────────────────────────────────────────────── */

static gint
cmd_task(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    const gchar *verb = (argc > 2) ? argv[2] : "list";

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    if (g_strcmp0(verb, "list") == 0) {
        JsonArray *tasks;
        guint i;

        reply = call(client, "task.list", NULL);
        if (reply == NULL)
            return EXIT_FAILURE;

        tasks = json_object_get_array_member(json_node_get_object(reply),
                                             "tasks");

        if (json_array_get_length(tasks) == 0) {
            g_print("No tasks.\n");
            return EXIT_SUCCESS;
        }

        g_print("%-26s %-12s %-12s %-10s %s\n", "ID", "FROM", "TO", "STATE",
                "WORK");

        for (i = 0; i < json_array_get_length(tasks); i++) {
            JsonObject *task = json_array_get_object_element(tasks, i);

            g_print("%-26s %-12s %-12s %-10s %s\n",
                    member_or(task, "id", "?"),
                    member_or(task, "origin", "?"),
                    member_or(task, "assignee", "?"),
                    member_or(task, "state", "?"),
                    member_or(task, "prompt", ""));
        }

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "show") == 0) {
        JsonObject *task;

        if (argc < 4) {
            g_printerr("Usage: clawtilla task show <task-id>\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "task.show",
                     build_payload("task", argv[3], NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        task = json_object_get_object_member(json_node_get_object(reply),
                                             "task");

        g_print("id:       %s\n", member_or(task, "id", "?"));
        g_print("from:     %s\n", member_or(task, "origin", "?"));
        g_print("to:       %s\n", member_or(task, "assignee", "?"));
        g_print("state:    %s\n", member_or(task, "state", "?"));
        g_print("work:     %s\n", member_or(task, "prompt", ""));

        if (json_object_has_member(task, "result"))
            g_print("result:   %s\n",
                    json_object_get_string_member(task, "result"));

        if (json_object_has_member(task, "reason"))
            g_print("reason:   %s\n",
                    json_object_get_string_member(task, "reason"));

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "cancel") == 0) {
        if (argc < 4) {
            g_printerr("Usage: clawtilla task cancel <task-id>\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "task.cancel",
                     build_payload("task", argv[3], NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("Cancelled %" G_GINT64_FORMAT " task(s).\n",
                json_object_get_int_member(json_node_get_object(reply),
                                           "cancelled"));

        return EXIT_SUCCESS;
    }

    g_printerr("clawtilla: unknown task verb '%s'\n", verb);
    return EXIT_FAILURE;
}

/* ── computers ───────────────────────────────────────────────────── */

static gint
cmd_computer(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    const gchar *verb = (argc > 2) ? argv[2] : NULL;
    const gchar *agent_id = (argc > 3) ? argv[3] : NULL;

    if (verb == NULL || agent_id == NULL) {
        g_printerr("Usage: clawtilla computer <exec|status> <agent> "
                   "[-- COMMAND...]\n");
        return EXIT_FAILURE;
    }

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    if (g_strcmp0(verb, "status") == 0) {
        reply = call(client, "computer.status",
                     build_payload("agent", agent_id, NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("state: %s\n\n%s\n",
                member_or(json_node_get_object(reply), "state", "?"),
                member_or(json_node_get_object(reply), "description", ""));

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "exec") == 0) {
        g_autofree gchar *command = NULL;
        gint start = 4;
        JsonObject *result;

        /* `--` is optional but conventional, so both forms work. */
        if (start < argc && g_strcmp0(argv[start], "--") == 0)
            start++;

        if (start >= argc) {
            g_printerr("Usage: clawtilla computer exec <agent> -- "
                       "COMMAND...\n");
            return EXIT_FAILURE;
        }

        command = g_strjoinv(" ", argv + start);

        reply = call(client, "computer.exec",
                     build_payload("agent", agent_id, "command", command,
                                   NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        result = json_node_get_object(reply);

        if (member_or(result, "stdout", NULL) != NULL)
            g_print("%s", json_object_get_string_member(result, "stdout"));

        if (member_or(result, "stderr", NULL) != NULL)
            g_printerr("%s", json_object_get_string_member(result, "stderr"));

        /*
         * The command's own exit status is this process's exit status, so
         * `clawtilla computer exec ... && something` behaves the way a
         * shell user expects.
         *
         * A reply without one is a failure rather than a success: reading
         * a missing member returns 0, and reporting success for a command
         * whose outcome is unknown is the worst of the options.
         */
        if (!json_object_has_member(result, "exit")) {
            g_printerr("clawtilla: the daemon did not report an exit "
                       "status\n");
            return EXIT_FAILURE;
        }

        return (gint)json_object_get_int_member(result, "exit");
    }

    g_printerr("clawtilla: unknown computer verb '%s'\n", verb);
    return EXIT_FAILURE;
}

/* ── config ──────────────────────────────────────────────────────── */

static gint
cmd_config(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(GError) error = NULL;
    const gchar *verb = (argc > 2) ? argv[2] : NULL;

    if (verb == NULL) {
        g_printerr("Usage: clawtilla config <show|validate|render|edit>\n");
        return EXIT_FAILURE;
    }

    /*
     * `edit` never goes near the daemon: it opens the file in $EDITOR and
     * then asks the daemon to reload it.  Editing through the socket would
     * mean reimplementing an editor over IPC.
     */
    if (g_strcmp0(verb, "edit") == 0) {
        g_autofree gchar *path = NULL;
        const gchar *editor = g_getenv("EDITOR");
        gint status = 0;

        path = clawt_expand_path(opt_config_path != NULL
                                 ? opt_config_path
                                 : "~/.clawtilla/config.yaml");

        if (editor == NULL)
            editor = "vi";

        /*
         * Built as an argv rather than a command line.  $EDITOR and the
         * config path both come from outside this process, and handing
         * them to a shell means a space or a semicolon in either one runs
         * something nobody asked for.  $EDITOR is still split on spaces,
         * because "code --wait" is a normal thing to have in it.
         */
        {
            g_auto(GStrv) editor_argv = NULL;
            g_autoptr(GPtrArray) spawn_argv = g_ptr_array_new();
            gsize i;

            if (!g_shell_parse_argv(editor, NULL, &editor_argv, &error)) {
                g_printerr("clawtilla: $EDITOR (%s) could not be parsed: "
                           "%s\n", editor, error->message);
                return EXIT_FAILURE;
            }

            for (i = 0; editor_argv[i] != NULL; i++)
                g_ptr_array_add(spawn_argv, editor_argv[i]);

            g_ptr_array_add(spawn_argv, path);
            g_ptr_array_add(spawn_argv, NULL);

            if (!g_spawn_sync(NULL, (gchar **)spawn_argv->pdata, NULL,
                              G_SPAWN_SEARCH_PATH | G_SPAWN_CHILD_INHERITS_STDIN,
                              NULL, NULL, NULL, NULL, &status, &error)) {
                g_printerr("clawtilla: could not run %s: %s\n", editor,
                           error->message);
                return EXIT_FAILURE;
            }
        }

        client = connect_to_daemon();

        if (client == NULL) {
            g_print("Edited %s. Start the daemon to apply it.\n", path);
            return EXIT_SUCCESS;
        }

        reply = call(client, "control.reload", NULL);

        if (reply == NULL) {
            g_printerr("clawtilla: the daemon kept its old configuration\n");
            return EXIT_FAILURE;
        }

        g_print("Reloaded.\n");
        return EXIT_SUCCESS;
    }

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    if (g_strcmp0(verb, "show") == 0) {
        reply = call(client, "config.show", NULL);
        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("%s", member_or(json_node_get_object(reply), "yaml", ""));
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "render") == 0) {
        if (argc < 4) {
            g_printerr("Usage: clawtilla config render <agent>\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "config.render",
                     build_payload("agent", argv[3], NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("%s", member_or(json_node_get_object(reply), "yaml", ""));
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "validate") == 0) {
        JsonObject *result;
        JsonArray *warnings;
        guint i;

        reply = call(client, "config.validate", NULL);
        if (reply == NULL)
            return EXIT_FAILURE;

        result = json_node_get_object(reply);
        warnings = json_object_get_array_member(result, "warnings");

        for (i = 0; i < json_array_get_length(warnings); i++)
            g_printerr("warning: %s\n",
                       json_array_get_string_element(warnings, i));

        if (!json_object_get_boolean_member(result, "valid")) {
            g_printerr("invalid: %s\n", member_or(result, "error", "?"));
            return EXIT_FAILURE;
        }

        g_print("Configuration is valid.\n");
        return EXIT_SUCCESS;
    }

    g_printerr("clawtilla: unknown config verb '%s'\n", verb);
    return EXIT_FAILURE;
}

static gint
cmd_image(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    JsonObject *root;
    JsonArray *images;
    const gchar *fallback;
    const gchar *last_group = NULL;
    guint i;

    (void)argc;
    (void)argv;

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    reply = call(client, "image.list", NULL);
    if (reply == NULL)
        return EXIT_FAILURE;

    root = json_node_get_object(reply);
    images = json_object_get_array_member(root, "images");
    fallback = member_or(root, "default", NULL);

    for (i = 0; i < json_array_get_length(images); i++) {
        JsonObject *image = json_array_get_object_element(images, i);
        const gchar *reference = member_or(image, "reference", "?");
        const gchar *group = member_or(image, "group", NULL);

        if (g_strcmp0(group, last_group) != 0) {
            g_print("%s%s\n", (last_group != NULL) ? "\n" : "",
                    group != NULL ? group : "Other");
            last_group = group;
        }

        /*
         * The default is marked rather than just being first: the user's
         * own images are listed above it, so position says nothing.
         */
        g_print("  %-46s %s%s\n", reference,
                (g_strcmp0(reference, fallback) == 0) ? "(default) " : "",
                member_or(image, "note", ""));
    }

    g_print("\nAny other reference podman can pull works too.\n");
    g_print("Add your own to defaults.container_images in the config.\n");

    return EXIT_SUCCESS;
}

static gint
cmd_model(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *providers;
    guint i;

    (void)argc;
    (void)argv;

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    reply = call(client, "model.list",
                 build_payload("refresh", "true", NULL));
    if (reply == NULL)
        return EXIT_FAILURE;

    providers = json_object_get_array_member(json_node_get_object(reply),
                                             "providers");

    for (i = 0; i < json_array_get_length(providers); i++) {
        JsonObject *provider = json_array_get_object_element(providers, i);
        JsonArray *models = json_object_get_array_member(provider, "models");
        guint j;

        g_print("%s  (%s)\n", member_or(provider, "id", "?"),
                member_or(provider, "note", ""));

        for (j = 0; j < json_array_get_length(models); j++) {
            JsonObject *model = json_array_get_object_element(models, j);

            g_print("    %-28s %s\n", member_or(model, "id", "?"),
                    member_or(model, "note", ""));
        }

        if (json_object_has_member(provider, "live") &&
            json_object_get_boolean_member(provider, "live"))
            g_print("    (listed by the provider just now)\n");

        if (json_object_get_boolean_member(provider, "open_ended"))
            g_print("    (any other model name this provider accepts)\n");

        g_print("\n");
    }

    return EXIT_SUCCESS;
}

static gint
cmd_integration(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    const gchar *verb = (argc > 2) ? argv[2] : "list";
    const gchar *agent_id = (argc > 3) ? argv[3] : NULL;
    guint i;

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    if (g_strcmp0(verb, "list") == 0) {
        JsonArray *integrations;

        reply = call(client, "integration.list",
                     build_payload("agent", agent_id, NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        integrations = json_object_get_array_member(
            json_node_get_object(reply), "integrations");

        for (i = 0; i < json_array_get_length(integrations); i++) {
            JsonObject *integration =
                json_array_get_object_element(integrations, i);

            g_print("%-10s %-4s %s\n", member_or(integration, "id", "?"),
                    json_object_get_boolean_member(integration, "enabled")
                        ? "on" : "-",
                    member_or(integration, "summary", ""));
        }

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "health") == 0) {
        JsonArray *checks;
        gint status = EXIT_SUCCESS;

        if (agent_id == NULL) {
            g_printerr("Usage: clawtilla integration health <agent> "
                       "[integration]\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "integration.health",
                     build_payload("agent", agent_id, "integration",
                                   argc > 4 ? argv[4] : NULL, NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        checks = json_object_get_array_member(json_node_get_object(reply),
                                              "checks");

        if (json_array_get_length(checks) == 0) {
            g_print("%s has no integrations enabled.\n", agent_id);
            return EXIT_SUCCESS;
        }

        for (i = 0; i < json_array_get_length(checks); i++) {
            JsonObject *check = json_array_get_object_element(checks, i);
            gboolean ok = json_object_get_boolean_member(check, "ok");

            g_print("%-10s %s%s%s\n", member_or(check, "id", "?"),
                    ok ? "ok" : "FAILED", ok ? "" : ": ",
                    ok ? "" : member_or(check, "error", ""));

            /*
             * A failing check is a non-zero exit, so this is usable in a
             * pre-flight script rather than only by eye.
             */
            if (!ok)
                status = EXIT_FAILURE;
        }

        return status;
    }

    g_printerr("clawtilla: unknown integration verb '%s'\n", verb);
    return EXIT_FAILURE;
}

static gint
cmd_plugin(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *plugins;
    guint i;

    if (argc > 2 && g_strcmp0(argv[2], "list") != 0) {
        g_printerr("Usage: clawtilla plugin list\n");
        return EXIT_FAILURE;
    }

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    reply = call(client, "plugin.list", NULL);
    if (reply == NULL)
        return EXIT_FAILURE;

    plugins = json_object_get_array_member(json_node_get_object(reply),
                                           "plugins");

    if (json_array_get_length(plugins) == 0) {
        g_print("No plugins loaded.\n");
        g_print("Put libclawt-plugin-<id>.so in "
                "~/.config/clawtilla/plugins/ or set CLAWT_PLUGIN_PATH.\n");
        return EXIT_SUCCESS;
    }

    for (i = 0; i < json_array_get_length(plugins); i++) {
        JsonObject *plugin = json_array_get_object_element(plugins, i);

        g_print("%-16s %-10s %-8s %s\n", member_or(plugin, "id", "?"),
                member_or(plugin, "version", "?"),
                json_object_get_boolean_member(plugin, "active")
                    ? "active" : "idle",
                member_or(plugin, "description", ""));
    }

    return EXIT_SUCCESS;
}

/* ── daemon control ──────────────────────────────────────────────── */

static gint
cmd_status(void)
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    JsonObject *status;

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    reply = call(client, "control.status", NULL);
    if (reply == NULL)
        return EXIT_FAILURE;

    status = json_node_get_object(reply);

    g_print("clawtillad %s\n", member_or(status, "version", "?"));
    g_print("config:    %s\n", member_or(status, "config", "?"));
    g_print("agents:    %" G_GINT64_FORMAT " (%" G_GINT64_FORMAT
            " connected)\n",
            json_object_get_int_member(status, "agents"),
            json_object_get_int_member(status, "connected"));
    g_print("clients:   %" G_GINT64_FORMAT "\n",
            json_object_get_int_member(status, "clients"));

    return EXIT_SUCCESS;
}

static gint
cmd_daemon(void)
{
    g_autoptr(ClawtDaemon) daemon = NULL;

    /*
     * The same daemon clawtillad runs, in the foreground.  Handy for a
     * quick session without installing a service, and it is the identical
     * code path rather than a second implementation that can drift.
     */
    daemon = clawt_daemon_new(opt_config_path, NULL);

    return clawt_daemon_run(daemon);
}

static gint
cmd_cp(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;

    if (argc < 4) {
        g_printerr("Usage: clawtilla cp <src> <dst>\n");
        g_printerr("  Either side may be <agent>:<path>.\n");
        g_printerr("  e.g. clawtilla cp ~/notes.org "
                   "researcher:/mnt/clawtilla/exchange/researcher/\n");
        return EXIT_FAILURE;
    }

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    reply = call(client, "computer.copy",
                 build_payload("src", argv[2], "dst", argv[3], NULL));

    return (reply != NULL) ? EXIT_SUCCESS : EXIT_FAILURE;
}

int
main(int argc, char *argv[])
{
    g_autoptr(GOptionContext) context = NULL;
    g_autoptr(GError) error = NULL;

    context = g_option_context_new("- orchestrate a fleet of libreclaw agents");
    g_option_context_add_main_entries(context, entries, NULL);
    g_option_context_set_description(context, usage_text);

    /*
     * Global options are parsed only from what comes BEFORE the verb.
     *
     * GOption scans the whole of argv, and it removes what it recognises
     * wherever it appears.  Letting it see the rest meant a message body
     * could be eaten -- `clawtilla send bot read --license and summarize`
     * printed the licence and never sent anything, exit status 0 -- and
     * that `-s` in the middle of a sentence silently became a socket
     * override.  Splitting first costs post-verb global flags, which is a
     * far smaller surprise than a message quietly not being sent.
     */
    {
        gint verb_index = find_verb(argc, argv);
        gint head_argc = (verb_index > 0) ? verb_index : argc;
        g_auto(GStrv) head = NULL;
        gint parsed_argc;
        gint i;

        head = g_new0(gchar *, (gsize)head_argc + 1);

        for (i = 0; i < head_argc; i++)
            head[i] = g_strdup(argv[i]);

        parsed_argc = head_argc;

        {
            gchar **head_argv = head;

            if (!g_option_context_parse(context, &parsed_argc, &head_argv,
                                        &error)) {
                g_printerr("clawtilla: %s\n", error->message);
                return EXIT_FAILURE;
            }
        }

        /* Anything left before the verb was not an option we know. */
        if (parsed_argc > 1 && verb_index > 0) {
            g_printerr("clawtilla: unexpected argument '%s' before the "
                       "command\n", head[1]);
            return EXIT_FAILURE;
        }

        if (verb_index > 0) {
            /* Hand the subcommand argv[0] plus the verb and its own args. */
            gint tail = argc - verb_index;

            for (i = 0; i < tail; i++)
                argv[i + 1] = argv[verb_index + i];

            argc = tail + 1;
            argv[argc] = NULL;
        } else {
            argc = parsed_argc;
        }
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

    if (opt_generate_service) {
        print_systemd_service();
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

    if (g_strcmp0(argv[1], "daemon") == 0)
        return cmd_daemon();

    if (g_strcmp0(argv[1], "status") == 0)
        return cmd_status();

    if (g_strcmp0(argv[1], "agent") == 0)
        return cmd_agent(argc, argv);

    if (g_strcmp0(argv[1], "send") == 0)
        return cmd_send(argc, argv);

    if (g_strcmp0(argv[1], "chat") == 0)
        return cmd_chat(argc, argv);

    if (g_strcmp0(argv[1], "mailbox") == 0)
        return cmd_mailbox(argc, argv);

    if (g_strcmp0(argv[1], "room") == 0)
        return cmd_room(argc, argv);

    if (g_strcmp0(argv[1], "task") == 0)
        return cmd_task(argc, argv);

    if (g_strcmp0(argv[1], "computer") == 0)
        return cmd_computer(argc, argv);

    if (g_strcmp0(argv[1], "cp") == 0)
        return cmd_cp(argc, argv);

    if (g_strcmp0(argv[1], "config") == 0)
        return cmd_config(argc, argv);

    if (g_strcmp0(argv[1], "plugin") == 0)
        return cmd_plugin(argc, argv);

    if (g_strcmp0(argv[1], "integration") == 0)
        return cmd_integration(argc, argv);

    if (g_strcmp0(argv[1], "image") == 0)
        return cmd_image(argc, argv);

    if (g_strcmp0(argv[1], "model") == 0)
        return cmd_model(argc, argv);

    g_printerr("clawtilla: unknown command '%s'\n", argv[1]);
    g_printerr("Run 'clawtilla --help' for usage.\n");
    return EXIT_FAILURE;
}
