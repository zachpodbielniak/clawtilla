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
#include <termios.h>

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
static gchar   *opt_profile = NULL;
static gchar   *opt_host = NULL;
static gint     opt_port = 0;
static gchar   *opt_token = NULL;

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
        "profile", 'p', 0, G_OPTION_ARG_STRING, &opt_profile,
        "Use a saved connection (see `clawtilla remote list`)", "NAME"
    },
    {
        "host", 'H', 0, G_OPTION_ARG_STRING, &opt_host,
        "Reach a daemon at this address instead of the local socket", "HOST"
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
 *
 * Everything except the command listing, which is rendered from the verb
 * table by build_usage_text() -- see the comment above it for why.
 */
static const gchar *usage_head =
    "clawtilla - orchestrate a fleet of libreclaw agents\n"
    "\n"
    "Usage:\n"
    "  clawtilla [OPTION...]\n"
    "  clawtilla <command> [ARGS...]\n"
    "\n"
    "Commands:\n";

/* Everything after the generated command listing. */
static const gchar *usage_tail =
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
    "  # Agents on disk that are not in the config\n"
    "  clawtilla agent discover\n"
    "  clawtilla agent import researcher   # adopt one, keeping its state\n"
    "  clawtilla agent forget researcher   # move it aside, delete nothing\n"
    "\n"
    "  # Bring in an agent you run standalone, and version the state dir\n"
    "  clawtilla agent import scribe --from ~/libreclaw/scribe\n"
    "  clawtilla agent git-init\n"
    "\n"
    "  # Put an agent on Matrix: sign in once, then choose its rooms\n"
    "  clawtilla integration add home matrix\n"
    "  clawtilla integration matrix-login home matrix.example.org researcher\n"
    "  clawtilla integration matrix-rooms home\n"
    "  clawtilla integration set home rooms=!abc:example.org\n"
    "  clawtilla integration scope home researcher\n"
    "\n"
    "  # Give the whole fleet an MCP server\n"
    "  clawtilla integration add github mcp scope=all \\\n"
    "      command=npx args=-y,@modelcontextprotocol/server-github\n"
    "\n"
    "  # What an agent has remembered\n"
    "  clawtilla memory list researcher\n"
    "  clawtilla memory search researcher \"deploy key\"\n"
    "\n"
    "  # Hand work to a chief-of-staff and watch it delegate\n"
    "  clawtilla send chief-of-staff \"summarize this week's commits\"\n"
    "  clawtilla task list\n"
    "\n"
    "  # Look at what is queued for an agent that is currently stopped\n"
    "  clawtilla mailbox list researcher\n"
    "\n"
    "  # What the fleet did overnight, and what it cost\n"
    "  clawtilla event list                # the daemon's own history\n"
    "  clawtilla event list researcher     # or one agent, room or task\n"
    "  clawtilla cost --days 7\n"
    "\n"
    "  # Reach a daemon on another machine\n"
    "  clawtilla daemon token                 # on that machine\n"
    "  clawtilla remote add workstation 100.72.0.41 --token TOKEN\n"
    "  clawtilla --profile workstation agent list\n"
    "\n"
    "Configuration is read from the path given with -c, otherwise from\n"
    "~/.clawtilla/config.yaml.\n"
    "\n"
    "clawtillad listens on this machine\'s tailnet address by default, so a\n"
    "machine on your tailnet usually needs nothing but the token.\n";

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


/* ── usage blocks ────────────────────────────────────────────────── */

/*
 * @text: the usage block to print
 * @asked_for: TRUE when the person typed --help, -h or help
 *
 * Asking for help is not an error, so it is answered on stdout with a
 * zero status; the same text printed *because* a command was wrong
 * belongs on stderr, after the complaint, with a non-zero one.
 *
 * Every print_*_usage() below goes through here rather than choosing a
 * stream itself, because each one choosing produced three different
 * wrong answers: `folders --help` was an error and printed to stderr,
 * `integration --help` and `routine --help` printed help to stderr while
 * exiting 0, and `connector`'s refusal put its usage on stdout where a
 * script capturing the error would never see it.  A rule five callers
 * apply is a rule four of them can get wrong.
 */
static void
print_usage_text(const gchar *text, gboolean asked_for)
{
    if (asked_for)
        g_print("%s", text);
    else
        g_printerr("%s", text);
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
    g_autoptr(ClawtConnection) connection = NULL;
    g_autoptr(GError) error = NULL;

    /*
     * --host beats --profile beats the local socket.  The explicit
     * address is last-resort and first-priority for the same reason: it
     * is what a person reaches for when a saved profile is the thing
     * they are trying to work around.
     */
    if (opt_host != NULL) {
        connection = clawt_connection_new_remote(
            opt_host, opt_host,
            opt_port > 0 ? (guint16)opt_port : CLAWT_DEFAULT_TCP_PORT,
            opt_token);
    } else if (opt_profile != NULL) {
        g_autoptr(GPtrArray) saved = clawt_connection_list_load(NULL, &error);
        ClawtConnection *found;

        if (saved == NULL) {
            g_printerr("clawtilla: %s\n", error->message);
            return NULL;
        }

        found = clawt_connection_list_find(saved, opt_profile);

        if (found == NULL) {
            g_printerr("clawtilla: there is no saved connection called "
                       "'%s'\n", opt_profile);
            g_printerr("  `clawtilla remote list` shows the ones there "
                       "are.\n");
            return NULL;
        }

        connection = clawt_connection_copy(found);
    } else {
        connection = clawt_connection_new_local("Local", opt_socket_path);
    }

    client = clawt_connection_create_client(connection);

    if (!clawt_client_connect(client, &error)) {
        g_autofree gchar *where = clawt_connection_describe(connection);

        g_printerr("clawtilla: %s (%s)\n", error->message, where);
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

/*
 * The relabel settings, walked from the library rather than listed here.
 *
 * A hand-written copy in a client is how the web client's list and the
 * schema's came to be two lists; parity layer 3 checks the enumeration
 * is used, and it can only check what nobody spelled out.
 */
static gboolean
relabel_is_known(const gchar *nick)
{
    guint i;

    for (i = 0; i < clawt_relabel_count(); i++) {
        if (g_strcmp0(clawt_relabel_nth_nick(i), nick) == 0)
            return TRUE;
    }

    return FALSE;
}

static void
print_relabel_help(void)
{
    guint i;

    g_printerr("  --relabel is one of:\n");

    for (i = 0; i < clawt_relabel_count(); i++)
        g_printerr("    %-8s %s\n", clawt_relabel_nth_nick(i),
                   clawt_relabel_nth_label(i));

    g_printerr("  Unset means '%s'. On SELinux a relabel is work podman "
               "does at every\n"
               "  start, walking the whole source -- so it cannot be asked "
               "for on a folder\n"
               "  this user does not own, and that is refused rather than "
               "saved.\n",
               clawt_enum_to_nick(CLAWT_TYPE_RELABEL, CLAWT_RELABEL_SHARED));
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

/*
 * Names every agent a command left running against a stale config.yaml.
 *
 * Seven handlers re-render the fleet's agent files, and any of them can
 * be refused for one agent while succeeding for the rest -- an
 * operator-typed `libreclaw:` block that redeclares a section clawtilla
 * renders itself is the ordinary cause, so this is a normal outcome of a
 * normal edit rather than an internal error.  The command still did what
 * it was asked; the exit status is non-zero because there is something
 * left for the person to fix.
 *
 * The sentence comes from libclawt so that the CLI, the GTK client and
 * the web client all say the same thing about it.
 *
 * Returns: %TRUE when anything was refused.
 */
static gboolean
report_refusals(JsonNode *reply)
{
    g_autofree gchar *text = clawt_ipc_reply_refusal_text(reply, NULL);

    if (text == NULL)
        return FALSE;

    g_printerr("clawtilla: %s\n", text);
    return TRUE;
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

/* The boolean twin, guarded the same way and for the same reason. */
static gboolean
member_flag(JsonObject *object, const gchar *key, gboolean fallback)
{
    if (object == NULL || !json_object_has_member(object, key))
        return fallback;

    if (json_node_get_value_type(json_object_get_member(object, key)) !=
        G_TYPE_BOOLEAN)
        return fallback;

    return json_object_get_boolean_member(object, key);
}

/*
 * A string array member, or %NULL when it is absent or is something else.
 */
static GStrv
string_array_member(JsonObject *object, const gchar *key)
{
    GPtrArray *out;
    JsonArray *array;
    guint i;
    guint length;

    if (object == NULL || !json_object_has_member(object, key) ||
        !JSON_NODE_HOLDS_ARRAY(json_object_get_member(object, key)))
        return NULL;

    array = json_object_get_array_member(object, key);
    length = json_array_get_length(array);
    out = g_ptr_array_new_with_free_func(g_free);

    for (i = 0; i < length; i++) {
        JsonNode *element = json_array_get_element(array, i);

        if (json_node_get_value_type(element) == G_TYPE_STRING)
            g_ptr_array_add(out, g_strdup(json_node_get_string(element)));
    }

    g_ptr_array_add(out, NULL);

    return (GStrv)g_ptr_array_free(out, FALSE);
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

/*
 * Every agent subverb, once.
 *
 * The branches below share cmd_agent's client and reply, so they cannot
 * become handler pointers without pulling the function apart -- but the
 * *names* still only need writing down once.  This table is read by the
 * usage text and by the guard in cmd_agent(), which is what keeps it
 * honest: a subverb left off it is refused rather than merely left
 * undocumented, so it cannot be added and forgotten.
 *
 * It was six.  `reset` was among them, and that one is the only way to
 * apply an identity change at all -- an AI CLI is never handed a system
 * prompt when it *resumes* a session, so restarting keeps the old one.
 */
static const struct {
    const gchar *name;
    const gchar *args;
    const gchar *summary;
} agent_verbs[] = {
    { "list", "", "every agent, its state and queue depth" },
    { "show", "<agent>", "one agent in full" },
    { "new", "", "design one by answering questions" },
    { "create", "--id <id> [...]", "add one from flags, and start it" },
    { "rm", "<agent> [--purge]", "remove it; --purge deletes its files" },
    { "start", "<agent>", "start it" },
    { "stop", "<agent>", "stop it" },
    { "restart", "<agent>", "stop it and start it again" },
    { "interrupt", "<agent>", "stop what it is doing now; leave it running" },
    { "reset", "<agent>", "clear its session, applying a new identity" },
    { "logs", "<agent>", "what it has written to its log" },
    { "set", "<agent> <key> <value>", "change one setting" },
    { "files", "<agent>", "the org files that become its prompt" },
    { "edit", "<agent> [FILE...]", "open those files in $EDITOR" },
    { "mount", "<verb> <agent> [...]", "folders shared with just this agent" },
    { "avatar", "<verb> <agent> [...]", "its profile picture" },
    { "git-init", "[agent]", "version an agent's state directory" },
    { "discover", "", "agents on disk the config does not know" },
    { "import", "<id> [--from DIR]", "adopt one, keeping its state" },
    { "forget", "<id>", "move it aside; nothing is deleted" },
};

static gboolean
agent_verb_is_known(const gchar *verb)
{
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(agent_verbs); i++) {
        if (g_strcmp0(verb, agent_verbs[i].name) == 0)
            return TRUE;
    }

    return FALSE;
}

/* @asked_for: see print_usage_text(). */
static void
print_agent_usage(gboolean asked_for)
{
    g_autoptr(GString) text = g_string_new("Usage: clawtilla agent <verb> "
                                           "[ARGS...]\n\n");
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(agent_verbs); i++) {
        g_autofree gchar *left = NULL;

        left = g_strdup_printf("%s%s%s", agent_verbs[i].name,
                               agent_verbs[i].args[0] != '\0' ? " " : "",
                               agent_verbs[i].args);
        g_string_append_printf(text, "  %-28s%s\n", left,
                               agent_verbs[i].summary);
    }

    print_usage_text(text->str, asked_for);
}

static gint
cmd_agent(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    const gchar *verb = (argc > 2) ? argv[2] : NULL;
    const gchar *target = (argc > 3) ? argv[3] : NULL;

    if (verb == NULL) {
        print_agent_usage(FALSE);
        return EXIT_FAILURE;
    }

    if (g_strcmp0(verb, "help") == 0 || g_strcmp0(verb, "--help") == 0 ||
        g_strcmp0(verb, "-h") == 0) {
        print_agent_usage(TRUE);
        return EXIT_SUCCESS;
    }

    /*
     * Refused here rather than at the bottom, so that a typo is answered
     * without a daemon -- and so that agent_verbs[] is load-bearing: a
     * branch below whose name is missing from it never runs.
     */
    if (!agent_verb_is_known(verb)) {
        g_printerr("clawtilla: unknown agent verb '%s'\n", verb);
        print_agent_usage(FALSE);
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

        /*
         * WORK, between STATE and QUEUE.
         *
         * `running` means the process is up, not that it is doing
         * anything: an agent thinking for four minutes and one idle for
         * four hours were the same word here.  The daemon has computed
         * `busy` and `peer` for exactly this since the field was added,
         * and the graphical clients have rendered them the whole time --
         * this one, the one used over ssh and from scripts, read
         * neither.
         */
        g_print("%-20s %-10s %-18s %-6s %-8s %s\n", "ID", "STATE", "WORK",
                "QUEUE", "LINK", "DESCRIPTION");

        for (i = 0; i < json_array_get_length(agents); i++) {
            JsonObject *agent = json_array_get_object_element(agents, i);
            g_autofree gchar *activity = clawt_agent_activity_label(
                json_object_has_member(agent, "busy") &&
                    json_object_get_boolean_member(agent, "busy"),
                member_or(agent, "peer", NULL));
            g_autofree gchar *hold = clawt_hold_label(
                json_object_has_member(agent, "held") &&
                    json_object_get_boolean_member(agent, "held"),
                json_object_has_member(agent, "draining") &&
                    json_object_get_boolean_member(agent, "draining"));
            /*
             * A hold outranks what the agent is doing, because it
             * changes what "working" means: a draining agent is
             * finishing its last turn and will not start another.
             */
            const gchar *work = (hold != NULL) ? hold : activity;

            g_print("%-20s %-10s %-18s %-6" G_GINT64_FORMAT " %-8s %s\n",
                    member_or(agent, "id", "?"),
                    member_or(agent, "state", "?"),
                    work != NULL ? work : "-",
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

        /* Likewise the VM's disk, size and address, which only a VM has. */
        if (json_object_has_member(agent, "vm_cpus")) {
            const gchar *disk = member_or(agent, "vm_image", NULL);
            const gchar *ssh_host = member_or(agent, "vm_ssh_host", NULL);

            /*
             * Said even when unset, and said loudly: a VM with no disk
             * defines, starts and boots nothing, and this is the only
             * place the CLI would ever show you why.
             */
            g_print("vm image:    %s\n",
                    (disk != NULL && *disk != '\0')
                        ? disk
                        : "(none -- the VM has nothing to boot)");
            g_print("vm:          %s core(s), %s MB RAM, %s GB disk\n",
                    member_or(agent, "vm_cpus", "?"),
                    member_or(agent, "vm_memory_mb", "?"),
                    member_or(agent, "vm_disk_gb", "?"));

            if (ssh_host != NULL && *ssh_host != '\0')
                g_print("vm ssh:      %s\n", ssh_host);

            /*
             * A desktop is built into the guest on its first boot and
             * takes a while, so it is worth saying that it was asked for
             * rather than leaving it to be inferred from the caps line.
             */
            if (json_object_has_member(agent, "desktop_enabled") &&
                json_object_get_boolean_member(agent, "desktop_enabled"))
                g_print("vm desktop:  GNOME, %s\n",
                        (json_object_has_member(agent, "desktop_input") &&
                         json_object_get_boolean_member(agent,
                                                        "desktop_input"))
                            ? "the agent may look and act"
                            : "the agent may look but not act");
        }
        g_print("computer:    %s\n", member_or(agent, "computer", "none"));
        g_print("can:         %s\n", member_or(agent, "caps", "-"));
        g_print("queue:       %" G_GINT64_FORMAT "\n",
                json_object_get_int_member(agent, "mailbox_depth"));

        /*
         * What it is doing, and which process is doing it.
         *
         * Always printed, including "idle" -- unlike the listing, where
         * a blank column is readable, a missing line here reads as a
         * field the CLI does not know about.  That is the question this
         * whole block exists to answer, so it must not be silent.
         */
        {
            g_autofree gchar *activity = clawt_agent_activity_label(
                json_object_has_member(agent, "busy") &&
                    json_object_get_boolean_member(agent, "busy"),
                member_or(agent, "peer", NULL));
            g_autofree gchar *hold = clawt_hold_label(
                json_object_has_member(agent, "held") &&
                    json_object_get_boolean_member(agent, "held"),
                json_object_has_member(agent, "draining") &&
                    json_object_get_boolean_member(agent, "draining"));

            g_print("work:        %s\n",
                    hold != NULL ? hold
                                 : (activity != NULL ? activity : "idle"));
        }

        /*
         * The process, when there is one.  `state` says whether an agent
         * is meant to be running; this says what is actually serving it,
         * and the two disagreeing is a real and previously undiagnosable
         * condition.
         */
        if (json_object_has_member(agent, "pid")) {
            gint64 uptime = json_object_get_int_member(agent, "uptime");

            g_print("pid:         %" G_GINT64_FORMAT "\n",
                    json_object_get_int_member(agent, "pid"));
            g_print("uptime:      %" G_GINT64_FORMAT "h %" G_GINT64_FORMAT
                    "m %" G_GINT64_FORMAT "s\n",
                    uptime / 3600, (uptime % 3600) / 60, uptime % 60);
        }

        /*
         * And whether that process is the first one.  Said only when it
         * is not: a respawn count of zero is the ordinary case and does
         * not need a line, while any other number is the thing somebody
         * chasing a flapping agent came here to find.
         */
        if (json_object_has_member(agent, "restarts") &&
            json_object_get_int_member(agent, "restarts") > 0)
            g_print("restarts:    %" G_GINT64_FORMAT " (the daemon has "
                    "replaced this agent's process)\n",
                    json_object_get_int_member(agent, "restarts"));

        if (json_object_has_member(agent, "credentials")) {
            JsonObject *credentials =
                json_object_get_object_member(agent, "credentials");
            g_autoptr(GList) names = json_object_get_members(credentials);
            GList *l;

            for (l = names; l != NULL; l = l->next)
                g_print("credential:  %s -> %s\n", (const gchar *)l->data,
                        json_object_get_string_member(credentials, l->data));
        }

        /*
         * What the persona costs.  The size is always printed and the
         * breakdown only when there is something to say -- a per-file
         * table on every agent is noise, and noise is what stops the one
         * that matters from being read.
         */
        if (json_object_has_member(json_node_get_object(reply), "identity")) {
            JsonObject *identity = json_object_get_object_member(
                json_node_get_object(reply), "identity");
            gint64 bytes = json_object_get_int_member(identity, "bytes");
            gint64 limit = json_object_get_int_member(identity, "limit");

            if (bytes > 0)
                g_print("identity:    %" G_GINT64_FORMAT " bytes of the "
                        "%" G_GINT64_FORMAT " a command-line argument "
                        "holds\n", bytes, limit);

            if (json_object_has_member(identity, "verdict")) {
                JsonArray *files =
                    json_object_get_array_member(identity, "files");
                guint i;

                g_printerr("\nwarning: %s.\n",
                           json_object_get_string_member(identity,
                                                         "verdict"));

                for (i = 0; i < json_array_get_length(files); i++) {
                    JsonObject *file = json_array_get_object_element(files, i);

                    g_printerr("  %-20s %" G_GINT64_FORMAT "%s\n",
                               json_object_get_string_member(file, "name"),
                               json_object_get_int_member(file, "bytes"),
                               json_object_get_boolean_member(file, "present")
                                   ? "" : "  (not in the workspace)");
                }

                g_printerr("\n");
            }
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
        const gchar *pinned_id = NULL;
        const gchar *pinned_name = NULL;
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

            /*
             * Pinned, not suggested: a model told to design an agent
             * renames it as a matter of course, and the agent then
             * appears under a name nobody chose.
             */
            if (g_strcmp0(argv[i], "--id") == 0 && i + 1 < argc) {
                pinned_id = argv[++i];
                continue;
            }

            if (g_strcmp0(argv[i], "--name") == 0 && i + 1 < argc) {
                pinned_name = argv[++i];
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
            g_printerr("  --id <id> and --name <name> keep what you "
                       "choose; the model fills in the rest.\n");
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

        if (pinned_id != NULL) {
            json_builder_set_member_name(builder, "id");
            json_builder_add_string_value(builder, pinned_id);
        }

        if (pinned_name != NULL) {
            json_builder_set_member_name(builder, "name");
            json_builder_add_string_value(builder, pinned_name);
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

            /*
             * One block, because `id` points *into* the reply.
             *
             * It was read out of `created` in a block of its own and
             * then printed after that block had ended, which unrefs the
             * node and frees the string it was still naming. It printed
             * the right thing every time, which is how a use-after-free
             * survives being looked at.
             */
            {
                g_autoptr(JsonNode) created =
                    call(client, "design.commit",
                         build_payload("draft", draft, NULL));
                JsonObject *committed;
                const gchar *failure;

                if (created == NULL)
                    return EXIT_FAILURE;

                committed = json_node_get_object(created);
                id = member_or(committed, "id", id);
                failure = member_or(committed, "start_error", NULL);

                g_print("Created %s.\n", id);
                g_print("Read what it thinks it is: clawtilla agent edit "
                        "%s\n", id);

                /*
                 * What happened, rather than what to do next. The daemon
                 * starts a new agent now -- a computer is built at start,
                 * so one created and left alone had a config file and no
                 * machine -- and printing the old instruction would send
                 * somebody to run a command that has already run.
                 */
                if (failure != NULL) {
                    g_printerr("clawtilla: it did not start: %s\n", failure);
                    g_printerr("clawtilla: fix that, then: clawtilla agent "
                               "start %s\n", id);
                } else if (member_flag(committed, "started", FALSE)) {
                    g_print("Started it.\n");
                } else {
                    g_print("Start it with: clawtilla agent start %s\n", id);
                }

                if (report_refusals(created))
                    return EXIT_FAILURE;
            }
        }

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "create") == 0) {
        g_autoptr(JsonBuilder) builder = json_builder_new();
        gint i;

        json_builder_begin_object(builder);

        for (i = 3; i < argc; i++) {
            const gchar *flag = argv[i];

            g_autofree gchar *field = NULL;

            /*
             * A flag with no value of its own, so it cannot go through
             * the --key value loop below.
             */
            if (g_strcmp0(flag, "--no-start") == 0) {
                json_builder_set_member_name(builder, "start");
                json_builder_add_boolean_value(builder, FALSE);
                continue;
            }

            if (!g_str_has_prefix(flag, "--") || i + 1 >= argc) {
                g_printerr("clawtilla: unexpected argument '%s'\n", flag);
                g_printerr("Usage: clawtilla agent create --id <id> "
                           "[--name X] [--model X] [--computer X] "
                           "[--image X] [--vm-image PATH] [--no-start]\n");
                g_printerr("\n  --image     container image\n");
                g_printerr("  --vm-image  disk image for --computer vm; "
                           "required, because a VM with no disk boots "
                           "nothing\n");
                g_printerr("  --no-start  leave it stopped; its computer "
                           "is built when it first starts, so a VM agent "
                           "will have no machine yet\n");
                return EXIT_FAILURE;
            }

            /*
             * Dashes are accepted where the field has an underscore.
             * --vm-image is the spelling every other option here uses,
             * and the payload member is vm_image; sending the dashed form
             * verbatim produced a member the daemon has never heard of,
             * which it ignores -- so the image was silently dropped and
             * the agent created without one.
             */
            field = g_strdelimit(g_strdup(flag + 2), "-", '_');

            json_builder_set_member_name(builder, field);
            json_builder_add_string_value(builder, argv[++i]);
        }

        json_builder_end_object(builder);

        reply = call(client, "agent.create", json_builder_get_root(builder));
        if (reply == NULL)
            return EXIT_FAILURE;

        {
            JsonObject *result = json_node_get_object(reply);
            const gchar *created_id = member_or(result, "id", "?");
            const gchar *failure = member_or(result, "start_error", NULL);

            g_print("Created %s.\n", created_id);

            /*
             * The daemon starts it now, because its computer is built at
             * start and an agent created and left alone had a config
             * file and no machine. So this reports what happened rather
             * than instructing somebody to run what has already run.
             */
            if (failure != NULL) {
                g_printerr("clawtilla: it did not start: %s\n", failure);
                g_printerr("clawtilla: fix that, then: clawtilla agent "
                           "start %s\n", created_id);
                return EXIT_FAILURE;
            }

            if (member_flag(result, "started", FALSE))
                g_print("Started it.\n");
            else
                g_print("Start it with: clawtilla agent start %s\n",
                        created_id);
        }

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "rm") == 0) {
        gboolean with_computer = FALSE;
        gboolean purge = FALSE;
        gint i;

        if (target == NULL) {
            g_printerr("Usage: clawtilla agent rm <agent> "
                       "[--with-computer] [--purge]\n");
            g_printerr("\n  --with-computer  tear down its container "
                       "or VM\n");
            g_printerr("  --purge          delete everything it owns: "
                       "workspace, persona, mailbox,\n"
                       "                   memories, transcripts and "
                       "credentials. Not undoable.\n");
            return EXIT_FAILURE;
        }

        for (i = 4; i < argc; i++) {
            if (g_strcmp0(argv[i], "--with-computer") == 0)
                with_computer = TRUE;
            else if (g_strcmp0(argv[i], "--purge") == 0)
                purge = TRUE;
        }

        reply = call(client, "agent.remove",
                     build_payload("agent", target,
                                   "remove_computer",
                                   with_computer ? "true" : NULL,
                                   "remove_files", purge ? "true" : NULL,
                                   NULL));
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

            {
                const gchar *files = member_or(json_node_get_object(reply),
                                               "files", NULL);

                /*
                 * Said separately, because it is the half that cannot be
                 * undone -- and a purge that refused a path has to be
                 * reported rather than left looking like it worked.
                 */
                if (files != NULL && g_strcmp0(files, "removed") == 0) {
                    /*
                     * A linked workspace was never ours to delete, and
                     * this line used to claim otherwise -- "Its files
                     * are gone too" about a directory that is still
                     * exactly where somebody left it, for the mode
                     * chosen precisely so it would be.
                     */
                    if (json_object_has_member(json_node_get_object(reply),
                                               "linked_workspace"))
                        g_print("Its workspace was a link, so only the "
                                "link is gone.\n");
                    else
                        g_print("Its files are gone too.\n");
                }
                else if (files != NULL)
                    g_printerr("clawtilla: its files were kept: %s\n",
                               files);
            }
        }

        return EXIT_SUCCESS;
    }

    /*
     * Ending the work without ending the agent.
     *
     * `stop` takes the agent down along with its container or VM and
     * needs a `start` afterwards; this kills the AI CLI carrying out the
     * turn and everything it spawned, and leaves the agent connected
     * with its session and mailbox intact.
     */
    if (g_strcmp0(verb, "interrupt") == 0) {
        gint64 killed;

        if (target == NULL) {
            g_printerr("Usage: clawtilla agent interrupt <agent>\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "agent.interrupt",
                     build_payload("agent", target, NULL));

        if (reply == NULL)
            return EXIT_FAILURE;

        {
            JsonObject *result = json_node_get_object(reply);

            killed = json_object_has_member(result, "killed")
                     ? json_object_get_int_member(result, "killed") : 0;
        }

        /*
         * The count, because zero and several mean different things to
         * whoever ran this: nothing was running, or that much was. A
         * bare "interrupted" reads the same either way.
         */
        if (killed > 0)
            g_print("%s: stopped, %" G_GINT64_FORMAT " process(es) ended\n",
                    target, killed);
        else
            g_print("%s: between turns -- nothing was running to stop\n",
                    target);

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

    /*
     * Clearing an agent's AI session, which is the only way to make an
     * identity change reach a running agent.
     *
     * An AI CLI is not handed a system prompt when it resumes a session,
     * so editing SOUL.org -- or repointing persona.identity_files --
     * reaches the agent's files and not its prompt: it keeps the identity
     * it was created with. This existed only in the GTK client, so from
     * a terminal there was no supported way to apply an identity change
     * at all.
     */
    if (g_strcmp0(verb, "reset") == 0) {
        gint64 cleared;

        if (target == NULL) {
            g_printerr("Usage: clawtilla agent reset <agent>\n");
            g_printerr("  Clears its AI session so it reads its identity "
                       "files again.\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "agent.reset",
                     build_payload("agent", target, NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        cleared = json_object_has_member(json_node_get_object(reply),
                                         "sessions_cleared")
                  ? json_object_get_int_member(json_node_get_object(reply),
                                               "sessions_cleared")
                  : 0;

        g_print("%s: cleared %" G_GINT64_FORMAT " session(s)\n", target,
                cleared);

        /*
         * The old sessions directory is moved aside rather than deleted,
         * so the history is still readable. Worth saying, because
         * "cleared" otherwise reads as "gone".
         */
        g_print("Its previous sessions are kept beside the new ones.\n");

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
                       "<source> <target> [ro|rw] [--relabel WHICH]\n");
            g_printerr("       clawtilla agent mount rm <agent> <target>\n");
            g_printerr("  e.g. clawtilla agent mount add researcher "
                       "~/src /work rw\n");
            print_relabel_help();
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

            if (report_refusals(reply))
                return EXIT_FAILURE;

            return EXIT_SUCCESS;
        }

        if (g_strcmp0(action, "add") == 0) {
            const gchar *mode = "ro";
            const gchar *relabel = NULL;
            gint arg;

            if (argc < 7) {
                g_printerr("Usage: clawtilla agent mount add <agent> "
                           "<source> <target> [ro|rw] [--relabel WHICH]\n");
                print_relabel_help();
                return EXIT_FAILURE;
            }

            /*
             * --relabel, because the daemon's default is `shared` and a
             * shared relabel is podman walking every file under the
             * source with lsetxattr at every start.  On a source this
             * user does not own that cannot work, and until this flag
             * existed the CLI could add such a mount and could not
             * spell the fix its own refusal recommends.
             */
            for (arg = 7; arg < argc; arg++) {
                if (g_strcmp0(argv[arg], "--relabel") == 0 &&
                    arg + 1 < argc) {
                    relabel = argv[++arg];

                    if (!relabel_is_known(relabel)) {
                        g_printerr("clawtilla: unknown relabel '%s'\n",
                                   relabel);
                        print_relabel_help();
                        return EXIT_FAILURE;
                    }
                } else if (argv[arg][0] != '-') {
                    mode = argv[arg];
                }
            }

            reply = call(client, "agent.mount.add",
                         build_payload("agent", who,
                                       "source", argv[5],
                                       "target", argv[6],
                                       "mode", mode,
                                       "relabel", relabel,
                                       NULL));
            if (reply == NULL)
                return EXIT_FAILURE;

            g_print("%s now sees %s at %s.\n", who, argv[5], argv[6]);
            g_print("Takes effect when the agent next starts.\n");

            if (report_refusals(reply))
                return EXIT_FAILURE;

            return EXIT_SUCCESS;
        }

        g_printerr("clawtilla: unknown mount action '%s'\n", action);
        return EXIT_FAILURE;
    }

    if (g_strcmp0(verb, "avatar") == 0) {
        const gchar *action = (argc > 3) ? argv[3] : NULL;
        const gchar *who = (argc > 4) ? argv[4] : NULL;

        if (action == NULL || who == NULL) {
            g_printerr("Usage: clawtilla agent avatar show <agent> [FILE]\n");
            g_printerr("       clawtilla agent avatar set <agent> <file>\n");
            g_printerr("       clawtilla agent avatar clear <agent>\n");
            return EXIT_FAILURE;
        }

        if (g_strcmp0(action, "show") == 0) {
            JsonObject *result;
            const gchar *mime;
            const gchar *etag;
            const gchar *encoded;

            reply = call(client, "agent.avatar",
                        build_payload("agent", who, NULL));
            if (reply == NULL)
                return EXIT_FAILURE;

            result = json_node_get_object(reply);
            mime = member_or(result, "mime", "?");
            etag = member_or(result, "etag", "?");
            encoded = member_or(result, "base64", NULL);

            /*
             * A file argument writes the bytes; without one this only
             * reports what there is, since a terminal is not a useful
             * place to dump an image.
             */
            if (argc > 5) {
                g_autofree guchar *raw = NULL;
                gsize length = 0;
                g_autoptr(GError) write_error = NULL;

                if (encoded == NULL) {
                    g_printerr("clawtilla: %s has no profile picture\n",
                              who);
                    return EXIT_FAILURE;
                }

                raw = g_base64_decode(encoded, &length);

                if (!g_file_set_contents(argv[5], (const gchar *)raw,
                                         (gssize)length, &write_error)) {
                    g_printerr("clawtilla: %s\n", write_error->message);
                    return EXIT_FAILURE;
                }

                g_print("Wrote %" G_GSIZE_FORMAT " bytes to %s (%s)\n",
                       length, argv[5], mime);
                return EXIT_SUCCESS;
            }

            g_print("mime:  %s\n", mime);
            g_print("bytes: %" G_GINT64_FORMAT "\n",
                    json_object_get_int_member(result, "bytes"));
            g_print("etag:  %s\n", etag);

            return EXIT_SUCCESS;
        }

        if (g_strcmp0(action, "set") == 0) {
            g_autofree gchar *contents = NULL;
            g_autofree gchar *encoded = NULL;
            g_autoptr(GError) read_error = NULL;
            gsize length = 0;

            if (argc < 6) {
                g_printerr("Usage: clawtilla agent avatar set <agent> "
                          "<file>\n");
                return EXIT_FAILURE;
            }

            /*
             * Read and re-encoded here, never a path sent to the daemon:
             * agent.avatar_set takes bytes and only bytes, so a client
             * cannot ask it to read an arbitrary file off its own disk.
             */
            if (!g_file_get_contents(argv[5], &contents, &length,
                                     &read_error)) {
                g_printerr("clawtilla: %s\n", read_error->message);
                return EXIT_FAILURE;
            }

            encoded = g_base64_encode((const guchar *)contents, length);

            reply = call(client, "agent.avatar_set",
                        build_payload("agent", who, "data", encoded, NULL));
            if (reply == NULL)
                return EXIT_FAILURE;

            g_print("%s's profile picture is set.\n", who);
            return EXIT_SUCCESS;
        }

        if (g_strcmp0(action, "clear") == 0) {
            reply = call(client, "agent.avatar_clear",
                        build_payload("agent", who, NULL));
            if (reply == NULL)
                return EXIT_FAILURE;

            g_print("%s's profile picture is cleared.\n", who);
            return EXIT_SUCCESS;
        }

        g_printerr("clawtilla: unknown avatar action '%s'\n", action);
        return EXIT_FAILURE;
    }

    if (g_strcmp0(verb, "git-init") == 0) {
        reply = call(client, "state.git_init", NULL);

        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("%s is %s a git repository.\n",
                member_or(json_node_get_object(reply), "path", "?"),
                json_object_get_boolean_member(json_node_get_object(reply),
                                               "created")
                    ? "now" : "already");
        g_print("Wrote %s, which keeps credentials, tokens, databases and\n"
                "the exchange directory out of it. Read it before your "
                "first commit.\n",
                member_or(json_node_get_object(reply), "gitignore", "?"));

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "discover") == 0) {
        JsonArray *found;
        guint i;

        reply = call(client, "agent.discover", NULL);
        if (reply == NULL)
            return EXIT_FAILURE;

        found = json_object_get_array_member(json_node_get_object(reply),
                                             "found");

        if (json_array_get_length(found) == 0) {
            g_print("Nothing on disk that is not in the config.\n");
            return EXIT_SUCCESS;
        }

        g_print("Directories that look like agents but are not configured:\n\n");

        for (i = 0; i < json_array_get_length(found); i++) {
            JsonObject *one = json_array_get_object_element(found, i);
            JsonArray *holds = json_object_get_array_member(one, "holds");
            g_autoptr(GString) what = g_string_new(NULL);
            guint j;

            for (j = 0; j < json_array_get_length(holds); j++) {
                if (what->len > 0)
                    g_string_append(what, " ");

                g_string_append(what, json_array_get_string_element(holds, j));
            }

            g_print("%-24s %-10s %s\n",
                    member_or(one, "id", "?"),
                    member_or(one, "kind", "?"),
                    what->len > 0 ? what->str : "(empty)");
            g_print("  %s\n", member_or(one, "path", ""));
        }

        g_print("\nAdopt one with `clawtilla agent import <id>`, or put it\n"
                "out of the way with `clawtilla agent forget <id>`.\n");

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "import") == 0) {
        const gchar *from = NULL;
        const gchar *mode = "copy";
        gboolean keep_git = FALSE;
        gint arg;

        if (target == NULL) {
            g_printerr("Usage: clawtilla agent import <id> "
                       "[--from DIR | --link DIR | --git URL] "
                       "[--keep-git]\n");
            g_printerr("  --from   copy a standalone libreclaw agent in\n");
            g_printerr("  --link   use a directory where it is, without "
                       "copying it\n");
            g_printerr("  --git    clone a repository, as a submodule "
                       "where that is possible\n");
            g_printerr("  `clawtilla agent discover` lists what is already "
                       "on disk\n");
            return EXIT_FAILURE;
        }

        for (arg = 4; arg < argc; arg++) {
            if (g_strcmp0(argv[arg], "--from") == 0 && arg + 1 < argc) {
                from = argv[++arg];
            } else if (g_strcmp0(argv[arg], "--link") == 0 &&
                       arg + 1 < argc) {
                from = argv[++arg];
                mode = "link";
            } else if (g_strcmp0(argv[arg], "--git") == 0 && arg + 1 < argc) {
                from = argv[++arg];
                mode = "git";
            } else if (g_strcmp0(argv[arg], "--keep-git") == 0) {
                keep_git = TRUE;
            }
        }

        /*
         * Two different imports.  With --from this copies somebody
         * else's libreclaw workspace in; without it, it adopts a
         * directory clawtilla already has, which needs no copying at
         * all.
         */
        if (from != NULL) {
            reply = call(client, "agent.import",
                         build_payload("id", target, "from", from,
                                       "mode", mode,
                                       "keep_git", keep_git ? "true" : "false",
                                       NULL));

            if (reply == NULL)
                return EXIT_FAILURE;

            g_print("Imported %s into %s\n", target,
                    member_or(json_node_get_object(reply), "workspace", "?"));

            /*
             * The daemon's own sentence, not one built here. Whether a
             * git import became a submodule depends on the machine, so
             * the side that found out is the side that says.
             */
            g_print("  %s\n",
                    member_or(json_node_get_object(reply), "detail",
                              "done"));
            g_print("Check it over with `clawtilla agent show %s` before "
                    "starting it.\n", target);

            return EXIT_SUCCESS;
        }

        /*
         * Import is create with the same id: the workspace and the
         * state directory are already where the daemon looks for them,
         * so adopting one is a config entry and nothing else. There is
         * deliberately no second code path that could treat an imported
         * agent differently from a created one.
         */
        reply = call(client, "agent.create",
                     build_payload("id", target, NULL));

        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("Imported %s. Its workspace and mailbox were already "
                "there.\n", target);
        g_print("Check it over with `clawtilla agent show %s` before "
                "starting it.\n", target);

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "forget") == 0) {
        if (target == NULL) {
            g_printerr("Usage: clawtilla agent forget <id>\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "agent.forget",
                     build_payload("id", target, NULL));

        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("Moved aside: %s\n",
                member_or(json_node_get_object(reply), "moved", "(nothing)"));
        g_print("Nothing was deleted. Remove the .discarded directories "
                "yourself when you are sure.\n");

        return EXIT_SUCCESS;
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

                const gchar *marker = "      ";

                /*
                 * The marker column is the useful one: "prompt" is a
                 * file the agent reads every turn, "shared" is one
                 * clawtilla writes into as well as you, and blank is
                 * yours alone.
                 */
                if (json_object_get_boolean_member(file, "identity"))
                    marker = "prompt";
                else if (json_object_has_member(file, "generated") &&
                         json_object_get_boolean_member(file, "generated"))
                    marker = "shared";

                g_print("%-18s %s  %s\n",
                        json_object_get_string_member(file, "name"),
                        marker,
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

        /*
         * ...but the point of the call is the render, so a refusal means
         * the setting reached clawtilla.yaml and nothing the agent reads.
         */
        if (report_refusals(reply))
            return EXIT_FAILURE;

        /*
         * An AI CLI reads some things once, when its session starts, so a
         * change under a running agent reaches its files and not its
         * session -- and the agent then reports, accurately, not having
         * whatever was just granted.
         *
         * The advice differs by key because the remedy does: a tool list
         * is read at session start, so a restart is enough, while a
         * resumed session is never handed a system prompt at all and
         * only clearing it applies an identity change.
         */
        if (member_flag(json_node_get_object(reply), "restart_required",
                        FALSE)) {
            if (g_str_has_prefix(argv[4], "persona."))
                g_print("Run `clawtilla agent reset %s`: a resumed session "
                        "is never handed a system prompt, so a restart "
                        "alone keeps the old identity.\n", argv[3]);
            else
                g_print("Restart it: it lists its tools when it starts.\n");
        }

        return EXIT_SUCCESS;
    }

    /*
     * Only reachable for a verb agent_verbs[] lists and no branch above
     * handles -- the other half of the same drift, which the guard
     * cannot see.  Say that, rather than blaming what was typed.
     */
    g_printerr("clawtilla: 'agent %s' is listed but not implemented\n", verb);
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
        gboolean steered =
            json_object_has_member(payload, "steered") &&
            json_object_get_boolean_member(payload, "steered");
        const gchar *state =
            json_object_has_member(payload, "target_state")
            ? json_object_get_string_member(payload, "target_state") : NULL;

        /*
         * Says where it went, because "sent" is ambiguous when the
         * recipient is stopped -- the message is queued and will arrive,
         * and a person watching for a reply should know that.
         *
         * The steered case is checked first because it queues nothing at
         * all: without it a correction typed at a working agent printed
         * "nobody was queued", which is both wrong and the opposite of
         * reassuring.
         */
        if (steered)
            g_print("Held: %s is mid-turn. It will be sent when this turn "
                    "ends, and is not in the transcript until then.\n",
                    argv[2]);
        else if (queued == 0)
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

/* ── memories ────────────────────────────────────────────────────── */

/*
 * What an agent has written down.
 *
 * Read-only from here on purpose. Memories are formed in the agent's own
 * work, and a person adding one by hand would be putting words in its
 * mouth that it later reads back as its own conclusion.
 */
static gint
cmd_memory(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    const gchar *verb = (argc > 2) ? argv[2] : NULL;
    const gchar *target = (argc > 3) ? argv[3] : NULL;
    JsonArray *memories;
    guint i;

    if (verb == NULL || target == NULL ||
        (g_strcmp0(verb, "list") != 0 && g_strcmp0(verb, "search") != 0)) {
        g_printerr("Usage: clawtilla memory list <agent> [--category C]\n");
        g_printerr("       clawtilla memory search <agent> <query>\n");
        return EXIT_FAILURE;
    }

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    if (g_strcmp0(verb, "search") == 0) {
        if (argc < 5) {
            g_printerr("Usage: clawtilla memory search <agent> <query>\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "memory.search",
                     build_payload("agent", target, "query", argv[4], NULL));
    } else {
        const gchar *category = NULL;
        int arg;

        for (arg = 4; arg + 1 < argc; arg++) {
            if (g_strcmp0(argv[arg], "--category") == 0)
                category = argv[arg + 1];
        }

        reply = call(client, "memory.list",
                     build_payload("agent", target, "category", category,
                                   NULL));
    }

    if (reply == NULL)
        return EXIT_FAILURE;

    memories = json_object_get_array_member(json_node_get_object(reply),
                                            "memories");

    if (json_array_get_length(memories) == 0) {
        g_print("Nothing.\n");
        return EXIT_SUCCESS;
    }

    for (i = 0; i < json_array_get_length(memories); i++) {
        JsonObject *memory = json_array_get_object_element(memories, i);
        const gchar *summary = member_or(memory, "summary", NULL);

        g_print("%s  [%s/%s]%s\n",
                member_or(memory, "id", "?"),
                member_or(memory, "category", "?"),
                member_or(memory, "importance", "?"),
                json_object_has_member(memory, "pinned") &&
                json_object_get_boolean_member(memory, "pinned")
                    ? "  pinned" : "");
        g_print("  %s\n",
                summary != NULL ? summary : member_or(memory, "content", ""));
    }

    g_print("\n%" G_GINT64_FORMAT " remembered in total.\n",
            json_object_get_int_member(json_node_get_object(reply), "total"));

    return EXIT_SUCCESS;
}

/*
 * The fleet's shared folders.
 *
 * A verb of its own rather than a corner of `config`, because it is the
 * one fleet setting that is a *list* somebody edits repeatedly -- and
 * because `clawtilla folders add ~/source` is the whole point: sharing
 * a directory with every agent should not take a YAML editor.
 */
/*
 * A repeated flag's values as a JSON array member.
 */
static void
add_argv_array(JsonBuilder *builder, const gchar *name, GPtrArray *items)
{
    guint i;

    json_builder_set_member_name(builder, name);
    json_builder_begin_array(builder);

    for (i = 0; i < items->len; i++)
        json_builder_add_string_value(builder, g_ptr_array_index(items, i));

    json_builder_end_array(builder);
}

/*
 * Who a shared folder reaches, in a phrase.
 *
 * Said on every row rather than only on the scoped ones: a listing where
 * most rows say nothing and one says "teams: backend" invites reading
 * the silent ones as unknown rather than as everybody.
 */
static gchar *
describe_scope(JsonObject *mount)
{
    const gchar *scope = member_or(mount, "scope", "all");
    g_autoptr(GString) out = NULL;
    static const gchar *const keys[] = { "agents", "teams", NULL };
    gsize k;

    if (g_strcmp0(scope, "all") == 0)
        return g_strdup("every agent");

    if (g_strcmp0(scope, "none") == 0)
        return g_strdup("nobody");

    out = g_string_new(NULL);

    for (k = 0; keys[k] != NULL; k++) {
        JsonArray *items = json_object_has_member(mount, keys[k])
            ? json_object_get_array_member(mount, keys[k]) : NULL;
        guint i;

        for (i = 0; items != NULL && i < json_array_get_length(items); i++) {
            if (out->len > 0)
                g_string_append(out, ", ");

            g_string_append_printf(out, "%s %s",
                                   g_strcmp0(keys[k], "teams") == 0
                                       ? "team" : "agent",
                                   json_array_get_string_element(items, i));
        }
    }

    /*
     * A `selected` scope naming nothing reaches nobody, and saying so
     * matters: it is the state a half-finished edit leaves behind.
     */
    return out->len > 0 ? g_strdup(out->str) : g_strdup("nobody (no list)");
}

/* @asked_for: see print_usage_text(). */
static void
print_folders_usage(gboolean asked_for)
{
    static const gchar *text =
        "Usage: clawtilla folders [list|add|rm] [ARGS...]\n"
        "\n"
        "  list                        what every agent gets\n"
        "  add <path> [inside] [--ro] [--team T] [--agent A]\n"
        "                              [--relabel WHICH]\n"
        "  rm <inside>                 stop sharing one\n"
        "\n"
        "Without --team or --agent a folder goes to every agent that has a\n"
        "computer, including ones you make later. Either flag may repeat.\n"
        "\n"
        "--relabel decides what podman does to SELinux labels under the "
        "source at\n"
        "every start. Unset means shared, which relabels; a folder this "
        "user does\n"
        "not own cannot be relabelled and is refused rather than saved.\n"
        "\n"
        "Container, distrobox and VM agents get these. A host agent does "
        "not:\n"
        "there a mount is the confinement allowlist rather than a shared "
        "folder.\n"
        "\n"
        "Examples:\n"
        "  clawtilla folders add ~/source\n"
        "  clawtilla folders add ~/Documents/notes /work/notes --ro\n"
        "  clawtilla folders add ~/src/product /work/product --team "
        "backend\n";

    print_usage_text(text, asked_for);
}

static gint
cmd_folders(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    const gchar *verb = (argc > 2) ? argv[2] : "list";

    if (g_strcmp0(verb, "-h") == 0 || g_strcmp0(verb, "--help") == 0 ||
        g_strcmp0(verb, "help") == 0) {
        print_folders_usage(TRUE);
        return EXIT_SUCCESS;
    }

    /*
     * Every other verb in this file refuses a subverb it does not know;
     * this one fell through to `list`, so `folders lst` listed the
     * folders and looked like it had worked.  Checked before connecting,
     * because answering a typo needs no daemon.
     */
    if (g_strcmp0(verb, "list") != 0 && g_strcmp0(verb, "add") != 0 &&
        g_strcmp0(verb, "rm") != 0) {
        g_printerr("clawtilla: unknown folders verb '%s'\n", verb);
        print_folders_usage(FALSE);
        return EXIT_FAILURE;
    }

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    if (g_strcmp0(verb, "add") == 0) {
        const gchar *source = (argc > 3) ? argv[3] : NULL;
        const gchar *target = NULL;
        const gchar *mode = "rw";
        const gchar *relabel = NULL;
        g_autoptr(GPtrArray) teams = g_ptr_array_new();
        g_autoptr(GPtrArray) agents = g_ptr_array_new();
        gint arg;

        if (source == NULL) {
            g_printerr("Usage: clawtilla folders add <path> [inside] "
                       "[--ro] [--relabel WHICH]\n");
            print_relabel_help();
            return EXIT_FAILURE;
        }

        for (arg = 4; arg < argc; arg++) {
            if (g_strcmp0(argv[arg], "--ro") == 0)
                mode = "ro";
            else if (g_strcmp0(argv[arg], "--rw") == 0)
                mode = "rw";
            else if (g_strcmp0(argv[arg], "--relabel") == 0 &&
                     arg + 1 < argc) {
                relabel = argv[++arg];

                if (!relabel_is_known(relabel)) {
                    g_printerr("clawtilla: unknown relabel '%s'\n", relabel);
                    print_relabel_help();
                    return EXIT_FAILURE;
                }
            }
            else if (g_strcmp0(argv[arg], "--team") == 0 && arg + 1 < argc)
                g_ptr_array_add(teams, argv[++arg]);
            else if (g_strcmp0(argv[arg], "--agent") == 0 && arg + 1 < argc)
                g_ptr_array_add(agents, argv[++arg]);
            else if (target == NULL)
                target = argv[arg];
        }

        /*
         * The same path inside when none is given. An agent told about
         * ~/source finds it at a name that reads the same in both
         * places, so a note about a file is a note either of you can
         * follow.
         */
        {
            g_autoptr(JsonBuilder) builder = json_builder_new();

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "source");
            json_builder_add_string_value(builder, source);
            json_builder_set_member_name(builder, "target");
            json_builder_add_string_value(builder,
                                          target != NULL ? target : source);
            json_builder_set_member_name(builder, "mode");
            json_builder_add_string_value(builder, mode);

            if (relabel != NULL) {
                json_builder_set_member_name(builder, "relabel");
                json_builder_add_string_value(builder, relabel);
            }

            /*
             * The lists go on the frame and the *daemon* decides the
             * scope from them, rather than this client sending
             * "selected" itself. One place decides what naming a team
             * means, and it is the one both graphical clients also talk
             * to.
             */
            if (teams->len > 0)
                add_argv_array(builder, "teams", teams);

            if (agents->len > 0)
                add_argv_array(builder, "agents", agents);

            json_builder_end_object(builder);
            reply = call(client, "defaults.mount.add",
                         json_builder_get_root(builder));
        }

        if (reply == NULL)
            return EXIT_FAILURE;

        if (teams->len > 0 || agents->len > 0)
            g_print("Shared with the agents you named, and with any agent "
                    "that joins a\nteam you named.\n");
        else
            g_print("Shared with every agent that has a computer.\n");

        g_print("It reaches one when that agent is next started.\n");
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "rm") == 0) {
        const gchar *target = (argc > 3) ? argv[3] : NULL;

        if (target == NULL) {
            g_printerr("Usage: clawtilla folders rm <inside>\n");
            g_printerr("The path *inside* the computer, which is what "
                       "`folders list` shows second.\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "defaults.mount.remove",
                     build_payload("target", target, NULL));

        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("No longer shared.\n");
        return EXIT_SUCCESS;
    }

    reply = call(client, "defaults.mount.list", NULL);

    if (reply == NULL)
        return EXIT_FAILURE;

    {
        JsonArray *mounts = json_object_get_array_member(
            json_node_get_object(reply), "mounts");
        guint i;

        if (mounts == NULL || json_array_get_length(mounts) == 0) {
            g_print("Nothing is shared with every agent.\n");
            g_print("Add one with `clawtilla folders add ~/source`.\n");
            return EXIT_SUCCESS;
        }

        for (i = 0; i < json_array_get_length(mounts); i++) {
            JsonObject *mount = json_array_get_object_element(mounts, i);
            g_autofree gchar *who = describe_scope(mount);

            g_print("%-28s -> %-22s %-10s %s\n",
                    member_or(mount, "source", "?"),
                    member_or(mount, "target", "?"),
                    g_strcmp0(member_or(mount, "mode", "rw"), "ro") == 0
                        ? "read-only" : "writable",
                    who);
        }

        /*
         * A folder scoped to somebody who is not there. On stderr, so a
         * script reading the list is unaffected and a person running it
         * still sees why an agent has no directory.
         */
        {
            JsonArray *warnings = json_object_get_array_member(
                json_node_get_object(reply), "warnings");

            for (i = 0; warnings != NULL &&
                        i < json_array_get_length(warnings); i++)
                g_printerr("warning: %s\n",
                           json_array_get_string_element(warnings, i));
        }
    }

    return EXIT_SUCCESS;
}

static gint
cmd_team(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    const gchar *verb = (argc > 2) ? argv[2] : NULL;

    if (verb == NULL) {
        g_printerr("Usage: clawtilla team "
                   "<list|create|set|rm|assign> [ARGS...]\n");
        g_printerr("\n");
        g_printerr("  list                          teams, who leads them "
                   "and how many are running\n");
        g_printerr("  create <id> [DESCRIPTION]     add one\n");
        g_printerr("  set <id> <key> <value>        name, description or "
                   "color\n");
        g_printerr("  rm <id>                       remove it; its agents "
                   "become teamless\n");
        g_printerr("  assign <agent> <team> [lead]  put an agent on a "
                   "team\n");
        return EXIT_FAILURE;
    }

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    if (g_strcmp0(verb, "list") == 0) {
        JsonArray *teams;
        JsonArray *warnings;
        guint i;

        reply = call(client, "team.list", NULL);
        if (reply == NULL)
            return EXIT_FAILURE;

        teams = json_object_get_array_member(json_node_get_object(reply),
                                             "teams");

        if (json_array_get_length(teams) == 0)
            g_print("No teams yet. Create one with:\n"
                    "  clawtilla team create research \"reads sources\"\n");

        for (i = 0; i < json_array_get_length(teams); i++) {
            JsonObject *team = json_array_get_object_element(teams, i);
            const gchar *lead = member_or(team, "lead", NULL);

            g_print("%-16s %s\n", member_or(team, "id", "?"),
                    member_or(team, "description", ""));
            g_print("%-16s %" G_GINT64_FORMAT " of %" G_GINT64_FORMAT
                    " running, lead: %s\n", "",
                    json_object_get_int_member(team, "running"),
                    json_object_get_int_member(team, "total"),
                    lead != NULL ? lead : "nobody");
        }

        /*
         * Printed to stderr and always, because a team with two leads or
         * an agent on a team nobody declared is the sort of thing that
         * only shows up as work quietly going nowhere.
         */
        warnings = json_object_get_array_member(json_node_get_object(reply),
                                                "warnings");

        for (i = 0; i < json_array_get_length(warnings); i++)
            g_printerr("clawtilla: %s\n",
                       json_array_get_string_element(warnings, i));

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "create") == 0) {
        if (argc < 4) {
            g_printerr("Usage: clawtilla team create <id> [DESCRIPTION]\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "team.create",
                     build_payload("id", argv[3],
                                   "description",
                                   (argc > 4) ? argv[4] : NULL, NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("Created team %s.\n", argv[3]);

        if (argc <= 4)
            g_print("Give it a description -- it is what the chief of "
                    "staff reads to decide what belongs here:\n"
                    "  clawtilla team set %s description \"...\"\n",
                    argv[3]);

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "set") == 0) {
        if (argc < 6) {
            g_printerr("Usage: clawtilla team set <id> <key> <value>\n");
            g_printerr("  keys: name, description, color, order\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "team.set",
                     build_payload("team", argv[3], "key", argv[4],
                                   "value", argv[5], NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("%s: %s = %s\n", argv[3], argv[4], argv[5]);

        if (report_refusals(reply))
            return EXIT_FAILURE;

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "rm") == 0) {
        if (argc < 4) {
            g_printerr("Usage: clawtilla team rm <id>\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "team.remove",
                     build_payload("team", argv[3], NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("Removed team %s.\n", argv[3]);

        {
            gint64 orphaned = json_object_get_int_member(
                json_node_get_object(reply), "orphaned");

            /*
             * Said rather than left to be discovered. The agents are
             * fine and still running; they are simply on a team that is
             * no longer declared, which is where hand-offs start going
             * nowhere.
             */
            if (orphaned > 0)
                g_print("%" G_GINT64_FORMAT " agent%s still name%s it. "
                        "Put them somewhere:\n"
                        "  clawtilla team assign <agent> <team>\n",
                        orphaned, orphaned == 1 ? "" : "s",
                        orphaned == 1 ? "s" : "");
        }

        if (report_refusals(reply))
            return EXIT_FAILURE;

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "assign") == 0) {
        gboolean lead = (argc > 5 && g_strcmp0(argv[5], "lead") == 0);

        if (argc < 5) {
            g_printerr("Usage: clawtilla team assign <agent> <team> "
                       "[lead]\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "agent.set",
                     build_payload("agent", argv[3], "key", "team",
                                   "value", argv[4], NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        {
            g_autoptr(JsonNode) role = call(
                client, "agent.set",
                build_payload("agent", argv[3], "key", "team_role",
                              "value", lead ? "lead" : "member", NULL));

            if (role == NULL)
                return EXIT_FAILURE;

            g_print("%s is now %s of %s.\n", argv[3],
                    lead ? "the lead" : "a member", argv[4]);

            if (report_refusals(role))
                return EXIT_FAILURE;
        }

        return EXIT_SUCCESS;
    }

    g_printerr("clawtilla: unknown team command '%s'\n", verb);
    return EXIT_FAILURE;
}

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

            /*
             * The message count is the column that makes this readable:
             * a fleet accumulates a direct room per pair of agents and
             * most of them have never been used.
             */
            g_print("%-24s %4" G_GINT64_FORMAT "  %s\n",
                    member_or(room, "id", "?"),
                    json_object_has_member(room, "messages")
                        ? json_object_get_int_member(room, "messages") : 0,
                    list->str);
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


/*
 * clawtilla cost -- what the fleet has spent.
 *
 * Every agent is a libreclaw process, and libreclaw has recorded a row
 * per AI turn all along.  Nothing had ever read them, so the one
 * question an operator running a paid fleet asks first had no answer
 * anywhere in the product.
 */
static gint
cmd_cost(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(JsonBuilder) builder = NULL;
    /* Not autoptr: clawt_client_request() takes the payload (transfer
     * full), so releasing it here as well is a second unref. */
    JsonNode *payload;
    JsonObject *root;
    JsonObject *total;
    JsonArray *agents;
    gint64 since = 0;
    gint64 days = 0;
    const gchar *only_agent = NULL;
    gdouble budget;
    guint i;
    gint shown = 0;

    /*
     * Parsed here rather than through GOption because every other verb
     * in this file does the same: the subcommand owns its own words.
     */
    for (i = 2; i < (guint)argc; i++) {
        if (g_strcmp0(argv[i], "--today") == 0) {
            GDateTime *now = g_date_time_new_now_local();
            GDateTime *midnight = g_date_time_new_local(
                g_date_time_get_year(now), g_date_time_get_month(now),
                g_date_time_get_day_of_month(now), 0, 0, 0.0);

            since = g_date_time_to_unix(midnight);
            g_date_time_unref(midnight);
            g_date_time_unref(now);
        } else if (g_strcmp0(argv[i], "--days") == 0 && i + 1 < (guint)argc) {
            days = g_ascii_strtoll(argv[++i], NULL, 10);
            if (days > 0)
                since = (g_get_real_time() / G_USEC_PER_SEC) - days * 86400;
        } else if (g_strcmp0(argv[i], "--agent") == 0 && i + 1 < (guint)argc) {
            only_agent = argv[++i];
        } else if (g_strcmp0(argv[i], "-h") == 0 ||
                   g_strcmp0(argv[i], "--help") == 0) {
            g_print("Usage: clawtilla cost [--today | --days N] "
                    "[--agent ID]\n"
                    "\n"
                    "What the fleet has spent, per agent and in total.\n"
                    "\n"
                    "Examples:\n"
                    "  clawtilla cost                # everything on record\n"
                    "  clawtilla cost --today        # since local midnight\n"
                    "  clawtilla cost --days 7       # the last week\n"
                    "  clawtilla cost --agent chief  # one agent\n");
            return EXIT_SUCCESS;
        } else {
            g_printerr("clawtilla cost: unknown option '%s'\n", argv[i]);
            return EXIT_FAILURE;
        }
    }

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "since");
    json_builder_add_int_value(builder, since);
    json_builder_end_object(builder);
    payload = json_builder_get_root(builder);

    reply = call(client, "usage.summary", payload);
    if (reply == NULL)
        return EXIT_FAILURE;

    root = json_node_get_object(reply);
    agents = json_object_get_array_member(root, "agents");
    total = json_object_get_object_member(root, "total");
    budget = json_object_get_double_member_with_default(root,
                                                        "task_budget_usd", 0.0);

    g_print("%-20s %8s %12s %12s %12s\n",
            "AGENT", "TURNS", "IN", "OUT", "COST");

    for (i = 0; i < json_array_get_length(agents); i++) {
        JsonObject *a = json_array_get_object_element(agents, i);
        const gchar *id = member_or(a, "id", "?");
        g_autofree gchar *cost = NULL;

        if (only_agent != NULL && g_strcmp0(id, only_agent) != 0)
            continue;

        cost = clawt_usage_format_cost(
            json_object_get_int_member_with_default(a, "cost_micros", 0));

        g_print("%-20s %8" G_GINT64_FORMAT " %12" G_GINT64_FORMAT
                " %12" G_GINT64_FORMAT " %12s\n",
                id,
                json_object_get_int_member_with_default(a, "turns", 0),
                json_object_get_int_member_with_default(a, "input_tokens", 0),
                json_object_get_int_member_with_default(a, "output_tokens", 0),
                cost);
        shown++;
    }

    if (shown == 0) {
        g_print("(no agents)\n");
        return EXIT_SUCCESS;
    }

    /*
     * The fleet line is printed even when one agent was asked for,
     * because "is this agent most of the bill" is the reason somebody
     * narrows the report in the first place.
     */
    {
        g_autofree gchar *cost = clawt_usage_format_cost(
            json_object_get_int_member_with_default(total, "cost_micros", 0));

        g_print("%-20s %8" G_GINT64_FORMAT " %12" G_GINT64_FORMAT
                " %12" G_GINT64_FORMAT " %12s\n",
                "fleet",
                json_object_get_int_member_with_default(total, "turns", 0),
                json_object_get_int_member_with_default(total, "input_tokens",
                                                        0),
                json_object_get_int_member_with_default(total, "output_tokens",
                                                        0),
                cost);
    }

    if (since > 0) {
        g_autoptr(GDateTime) from = g_date_time_new_from_unix_local(since);
        g_autofree gchar *stamp = g_date_time_format(from, "%Y-%m-%d %H:%M");

        g_print("\nSince %s.\n", stamp);
    }

    if (budget > 0.0)
        g_print("Each delegated task may spend $%.2f "
                "(orchestration.task_budget_usd).\n", budget);

    /*
     * Said plainly, because the alternative is somebody reconciling this
     * against a provider invoice and concluding the fleet is lying.
     * Input tokens are what the CLI reported as *new* input; cache reads
     * are billed and are not in that number, which is why the cost can
     * look large next to the token counts beside it.
     */
    g_print("\nCost is the figure the provider reported for each turn.\n"
            "IN counts new input tokens only -- cached context is billed\n"
            "but is not reported as tokens, so IN understates the context.\n");

    return EXIT_SUCCESS;
}


/* ── tasks ───────────────────────────────────────────────────────── */

/*
 * The event log from a terminal.
 *
 * ClawtEventLog has recorded every published event since the daemon was
 * written, and `event.list` exists so that diagnosing a message loop is
 * a control rather than sqlite3 and grep on the host.  Both graphical
 * clients read it; from a terminal it was unreachable -- which is the
 * one place somebody is standing when they have ssh'd in to find out
 * what a fleet did overnight.
 */
static gint
cmd_event(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    const gchar *verb = (argc > 2) ? argv[2] : "list";
    const gchar *subject = (argc > 3) ? argv[3] : NULL;
    JsonArray *events;
    guint i;

    if (g_strcmp0(verb, "list") != 0) {
        g_printerr("Usage: clawtilla event list [subject]\n"
                   "\n"
                   "A subject is an agent, a room or a task. Without one "
                   "the whole fleet is\nshown, which is the case that "
                   "sends people to the shell.\n");
        return EXIT_FAILURE;
    }

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    reply = call(client, "event.list",
                 (subject != NULL)
                     ? build_payload("subject", subject, NULL) : NULL);
    if (reply == NULL)
        return EXIT_FAILURE;

    events = json_object_get_array_member(json_node_get_object(reply),
                                          "events");

    if (json_array_get_length(events) == 0) {
        g_print("Nothing recorded.\n");
        return EXIT_SUCCESS;
    }

    g_print("%-20s %-22s %s\n", "WHEN", "KIND", "SUBJECT");

    for (i = 0; i < json_array_get_length(events); i++) {
        JsonObject *event = json_array_get_object_element(events, i);
        gint64 ts = json_object_has_member(event, "ts")
                    ? json_object_get_int_member(event, "ts") : 0;
        g_autoptr(GDateTime) when = NULL;
        g_autofree gchar *stamp = NULL;

        /*
         * Microseconds, as the event carries them.  Printing the raw
         * number would make the one column somebody scans useless.
         */
        when = (ts > 0)
               ? g_date_time_new_from_unix_local(ts / G_USEC_PER_SEC)
               : NULL;
        stamp = (when != NULL) ? g_date_time_format(when, "%Y-%m-%d %H:%M:%S")
                               : g_strdup("-");

        g_print("%-20s %-22s %s\n", stamp,
                member_or(event, "kind", "?"),
                member_or(event, "subject", ""));
    }

    return EXIT_SUCCESS;
}

/*
 * The decision inbox from a terminal.
 *
 * Present here because the two graphical clients are not the only way
 * somebody runs a fleet, and a decision an operator cannot see over ssh
 * is one that silently takes its default.
 */
static int
cmd_decision(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    const gchar *verb = (argc > 2) ? argv[2] : "list";

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    if (g_strcmp0(verb, "list") == 0) {
        JsonArray *items;
        guint i;
        gboolean all = (argc > 3 && g_strcmp0(argv[3], "--all") == 0);

        /*
         * `--all` widens the list to the settled ones.  Sent as the
         * string the payload builder takes rather than a boolean,
         * because clawt_ipc_payload_boolean() reads "false" as false --
         * and a second builder for one flag is a second thing to keep
         * in step.
         */
        reply = call(client, "decision.list",
                     all ? build_payload("open", "false", NULL) : NULL);
        if (reply == NULL)
            return EXIT_FAILURE;

        items = json_object_get_array_member(json_node_get_object(reply),
                                             "decisions");

        if (json_array_get_length(items) == 0) {
            /*
             * The bar is stated on the empty list too.  The failure mode
             * is agents being polite rather than selective, and an
             * operator who never sees one should know that is the
             * intended state rather than a broken feature.
             */
            g_print("Nothing is waiting on you.\n\n"
                    "Agents file a decision only when the branches "
                    "produce materially\ndifferent work, or the choice is "
                    "not cheaply reversible.\n");
            return EXIT_SUCCESS;
        }

        for (i = 0; i < json_array_get_length(items); i++) {
            JsonObject *decision = json_array_get_object_element(items, i);
            const gchar *reason = member_or(decision, "default_reason", "");

            g_print("%s  [%s]%s\n", member_or(decision, "id", "?"),
                    member_or(decision, "agent", "?"),
                    json_object_get_boolean_member(decision, "urgent")
                        ? "  closing soon" : "");
            g_print("  %s\n", member_or(decision, "question", ""));
            g_print("  going ahead with: %s%s%s\n",
                    member_or(decision, "default", "(none stated)"),
                    (*reason != '\0') ? " -- " : "", reason);

            if (json_object_get_boolean_member(decision,
                                               "settled_by_default"))
                g_print("  the deadline passed; answering changes "
                        "nothing\n");

            g_print("\n");
        }

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "answer") == 0) {
        if (argc < 5) {
            g_printerr("Usage: clawtilla decision answer <id> <answer>\n"
                       "\n"
                       "The answer is free text -- \"neither, do X\" is a "
                       "perfectly good answer.\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "decision.answer",
                     build_payload("decision", argv[3],
                                   "answer", argv[4], NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("Answered. %s has been told.\n",
                member_or(json_object_get_object_member(
                              json_node_get_object(reply), "decision"),
                          "agent", "the agent"));
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "dismiss") == 0) {
        if (argc < 4) {
            g_printerr("Usage: clawtilla decision dismiss <id>\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "decision.dismiss",
                     build_payload("decision", argv[3], NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("Dismissed. The agent carries on with its default.\n");
        return EXIT_SUCCESS;
    }

    g_printerr("Usage: clawtilla decision [list [--all] | answer <id> "
               "<answer> | dismiss <id>]\n");
    return EXIT_FAILURE;
}

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

        if (json_object_has_member(task, "progress_note"))
            g_print("latest:   %s\n",
                    json_object_get_string_member(task, "progress_note"));

        if (json_object_has_member(task, "parent"))
            g_print("under:    %s\n",
                    json_object_get_string_member(task, "parent"));

        /*
         * The caveat goes last, where somebody reading a result has just
         * read it.  "They reported this" and "they stopped talking and
         * this is the last thing they wrote" need different follow-ups,
         * and a state name alone cannot tell them apart.
         */
        if (json_object_has_member(task, "result_inferred"))
            g_print("note:     nobody reported this finished; the result "
                    "is the last thing %s wrote before its turn ended\n",
                    member_or(task, "assignee", "the assignee"));

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

/*
 * Whether the agent may act on its screen right now.
 *
 * **Fails open, deliberately.** Every path that cannot get an answer --
 * no daemon, a refused connection, a reply in a shape this does not
 * understand -- allows the call. This is cooperation between an operator
 * and their own agent, not a boundary against a hostile one: nothing
 * here is confining a program or protecting a secret, and the worst a
 * lost race does is put one click somewhere unexpected on a screen the
 * person is already looking at. A daemon restart that bricked every
 * desktop tool in the fleet mid-turn would cost a great deal more.
 *
 * The permission that is *not* like this -- computer.desktop.allow_input
 * -- is enforced through the tool list, which is fixed before the relay
 * starts and so cannot fail open.
 */
static gboolean
desktop_control_gate(const gchar *tool, gchar **refusal, gpointer user_data)
{
    const gchar *agent_id = user_data;
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(GError) error = NULL;
    JsonObject *result;

    *refusal = NULL;

    /*
     * Only the tools that do something. A screenshot during a takeover
     * is exactly what an agent should still be able to take -- one that
     * can watch can wait usefully, and one that has been blinded as well
     * simply stops.
     */
    if (!clawt_desktop_tool_is_acting(tool))
        return TRUE;

    client = connect_to_daemon();

    if (client == NULL)
        return TRUE;

    reply = clawt_client_request(
        client, "computer.control",
        build_payload("agent", agent_id, "tool", tool, NULL), &error);

    if (reply == NULL || !JSON_NODE_HOLDS_OBJECT(reply))
        return TRUE;

    result = json_node_get_object(reply);

    if (result == NULL || !json_object_has_member(result, "allowed"))
        return TRUE;

    if (json_object_get_boolean_member(result, "allowed"))
        return TRUE;

    *refusal = g_strdup(
        json_object_get_string_member_with_default(
            result, "refusal",
            "clawtilla is not allowing that right now."));

    return FALSE;
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
        g_printerr("Usage: clawtilla computer "
                   "<exec|status|screen|takeover|release|start|stop|restart|"
                   "rebuild|desktop-mcp> <agent> [-- COMMAND...]\n");
        g_printerr("\n");
        g_printerr("  start <agent>          power the machine on\n");
        g_printerr("  stop <agent> [--remove]\n");
        g_printerr("                         power it off; --remove is "
                   "needed when stopping\n");
        g_printerr("                         destroys it, which is a "
                   "container's default\n");
        g_printerr("  restart <agent> [--remove]\n");
        g_printerr("                         both, in that order\n");
        g_printerr("\n");
        g_printerr("\n");
        g_printerr("  screen <agent>         what is on its screen, and "
                   "who is holding it\n");
        g_printerr("  takeover <agent>       stop the agent acting on the "
                   "screen and drive it\n");
        g_printerr("  release <agent>        give it back; this also "
                   "settles a request for hands\n");
        g_printerr("\n");
        g_printerr("  e.g. clawtilla computer restart oxpecker\n");
        return EXIT_FAILURE;
    }

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    /*
     * Not a verb a person runs.  It is what an agent's .mcp.json names,
     * and it speaks MCP on stdin and stdout -- so nothing here may print
     * to stdout except the protocol itself.
     */
    if (g_strcmp0(verb, "desktop-mcp") == 0) {
        g_auto(GStrv) relay_argv = NULL;
        g_auto(GStrv) tools = NULL;
        JsonObject *result;

        reply = call(client, "computer.desktop",
                     build_payload("agent", agent_id, NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        result = json_node_get_object(reply);
        relay_argv = string_array_member(result, "argv");
        tools = string_array_member(result, "tools");

        if (relay_argv == NULL || relay_argv[0] == NULL) {
            g_printerr("clawtilla: the daemon gave no way to reach that "
                       "agent's desktop\n");
            return EXIT_FAILURE;
        }

        /*
         * The daemon connection is dropped first.  The relay runs for as
         * long as the agent does, and holding an idle IPC connection open
         * all that time would keep a client slot for nothing.
         */
        g_clear_object(&client);

        /*
         * The gate reconnects per call rather than holding this one, for
         * the same reason: an acting tool is a handful of calls a
         * minute at most, and a connection kept open for hours to answer
         * one question is a client slot spent on nothing.
         */
        return clawt_desktop_relay_run_gated(relay_argv, tools,
                                             desktop_control_gate,
                                             (gpointer)agent_id);
    }

    if (g_strcmp0(verb, "screen") == 0 ||
        g_strcmp0(verb, "takeover") == 0 ||
        g_strcmp0(verb, "release") == 0) {
        JsonObject *screen;
        const gchar *kind;

        /*
         * Spelled out rather than pasted together from the verb, for
         * the reason the power verbs are: a frame kind built with
         * g_strconcat() is one `make parity` cannot see, and a verb
         * from the command line pasted into a frame name is a frame
         * name somebody else chose.
         */
        if (g_strcmp0(verb, "screen") == 0)
            kind = "computer.screen";
        else if (g_strcmp0(verb, "takeover") == 0)
            kind = "computer.takeover";
        else
            kind = "computer.release";

        reply = call(client, kind,
                     build_payload("agent", agent_id,
                                   "holder", "the command line", NULL));

        if (reply == NULL)
            return EXIT_FAILURE;

        screen = json_node_get_object(reply);

        g_print("type:     %s\n", member_or(screen, "type", "?"));
        g_print("watching: %" G_GINT64_FORMAT " client(s) at %"
                G_GINT64_FORMAT " fps\n",
                json_object_get_int_member_with_default(screen, "watchers", 0),
                json_object_get_int_member_with_default(screen, "fps", 0));

        if (json_object_get_boolean_member_with_default(screen, "held", FALSE))
            g_print("held by:  %s\n", member_or(screen, "holder", "somebody"));
        else
            g_print("held by:  nobody -- the agent may act on it\n");

        if (json_object_has_member(screen, "request"))
            g_print("asked:    %s\n", member_or(screen, "request", ""));

        if (json_object_has_member(screen, "viewer"))
            g_print("viewer:   %s\n", member_or(screen, "viewer", ""));

        if (json_object_has_member(screen, "error"))
            g_print("error:    %s\n", member_or(screen, "error", ""));

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "start") == 0 ||
        g_strcmp0(verb, "stop") == 0 ||
        g_strcmp0(verb, "restart") == 0) {
        g_autofree gchar *frame = g_strdup_printf("computer.%s", verb);
        g_autoptr(JsonBuilder) payload = json_builder_new();
        JsonObject *root;
        gboolean remove_it = FALSE;
        gint i;

        for (i = 4; i < argc; i++) {
            if (g_strcmp0(argv[i], "--remove") == 0)
                remove_it = TRUE;
        }

        /*
         * Built by hand rather than through build_payload(), which takes
         * strings: `remove` has to arrive as a boolean, and a string
         * "true" would read as absent and produce the refusal it was
         * passed to answer.
         */
        json_builder_begin_object(payload);
        json_builder_set_member_name(payload, "agent");
        json_builder_add_string_value(payload, agent_id);
        json_builder_set_member_name(payload, "remove");
        json_builder_add_boolean_value(payload, remove_it);
        json_builder_end_object(payload);

        reply = call(client, frame,
                     json_node_ref(json_builder_get_root(payload)));
        if (reply == NULL)
            return EXIT_FAILURE;

        root = json_node_get_object(reply);

        g_print("%s: %s\n", agent_id, member_or(root, "state", "?"));

        /*
         * Said afterwards as well as fenced beforehand.  Somebody who
         * passed --remove knew; somebody reading a script's output a
         * week later did not.
         *
         * Phrased as what this machine does rather than as what just
         * happened: a stop reaching a machine that was already gone
         * removed nothing, and announcing a destruction that did not
         * occur is its own kind of wrong.
         */
        if (json_object_has_member(root, "removes") &&
            json_object_get_boolean_member(root, "removes"))
            g_print("This machine does not survive a stop -- keep is false "
                    "-- so starting it builds a fresh one.\n");

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "rebuild") == 0) {
        JsonObject *root;
        const gchar *note;

        reply = call(client, "computer.rebuild",
                     build_payload("agent", agent_id, NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        root = json_node_get_object(reply);
        note = member_or(root, "note", NULL);

        /*
         * Said when there was nothing to tear down, because the two
         * outcomes look identical afterwards and one of them means the
         * guest had already gone.
         */
        if (note != NULL)
            g_print("Nothing to remove first (%s).\n", note);

        g_print("Rebuilt %s's computer. Its contents are gone; "
                "cloud-init runs again on the next start.\n", agent_id);

        return EXIT_SUCCESS;
    }

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

        /*
         * Sent as an array, not joined into a line.
         *
         * The shell has already split and unquoted these for us. Joining
         * them back up and letting the daemon re-split meant every layer
         * of quoting was consumed twice: `-- echo 'x\ny'` printed `xny`,
         * and `-- sh -c 'echo a; echo b'` became four arguments and ran
         * nothing at all.
         */
        {
            g_autoptr(JsonBuilder) builder = json_builder_new();
            gint i;

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "agent");
            json_builder_add_string_value(builder, agent_id);
            json_builder_set_member_name(builder, "argv");
            json_builder_begin_array(builder);

            for (i = start; i < argc; i++)
                json_builder_add_string_value(builder, argv[i]);

            json_builder_end_array(builder);
            json_builder_end_object(builder);

            reply = call(client, "computer.exec",
                         json_builder_get_root(builder));
        }
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

/*
 * What `control.reload` answered, turned into an exit status.
 *
 * A reload can succeed and still leave an agent behind: clawtilla refuses
 * to render a `libreclaw:` passthrough that redeclares a section it
 * renders itself, and that agent keeps the config.yaml it already had.
 * Printing "Reloaded." over the top of that was the whole problem -- it
 * read as "your edit was ignored" rather than "your edit was rejected,
 * here is why", and the reason was only ever in the daemon's journal.
 */
static gint
report_reload(JsonNode *reply)
{
    if (report_refusals(reply))
        return EXIT_FAILURE;

    g_print("Reloaded.\n");
    return EXIT_SUCCESS;
}

static gint
cmd_config(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(GError) error = NULL;
    const gchar *verb = (argc > 2) ? argv[2] : NULL;

    if (verb == NULL) {
        g_printerr("Usage: clawtilla config "
                   "<show|validate|render|edit|reload>\n");
        return EXIT_FAILURE;
    }

    /*
     * Reload without an editor.
     *
     * `edit` has always ended by sending control.reload, and it was the
     * only verb that did -- so reloading meant opening $EDITOR, which
     * edits the file on *this* machine.  Against a daemon on another one
     * that is not a remedy: the config to change is over there, and the
     * only way to apply an edit was to restart the daemon by hand.
     *
     * The case that made it matter: `agent set` on the key an agent was
     * shadowed for wrote the value and left the agent disabled until the
     * config was loaded again.  That is fixed at the source now, and this
     * is still the verb for every other reason a config changes -- an
     * edit made over ssh, or by anything that is not clawtilla.
     */
    if (g_strcmp0(verb, "reload") == 0) {
        client = connect_to_daemon();

        if (client == NULL)
            return EXIT_FAILURE;

        reply = call(client, "control.reload", NULL);

        if (reply == NULL) {
            g_printerr("clawtilla: the daemon kept its old configuration\n");
            return EXIT_FAILURE;
        }

        report_refusals(reply);
        g_print("Reloaded.\n");

        return EXIT_SUCCESS;
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

        return report_reload(reply);
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


/* ── Cloud images for VMs ────────────────────────────────────────── */

typedef struct {
    const gchar *name;
    GMainLoop   *loop;
    gint         status;
} ImageWatch;

static gchar *
human_bytes(gint64 bytes)
{
    if (bytes >= 1024 * 1024 * 1024)
        return g_strdup_printf("%.1f GB", bytes / (1024.0 * 1024.0 * 1024.0));

    if (bytes >= 1024 * 1024)
        return g_strdup_printf("%.0f MB", bytes / (1024.0 * 1024.0));

    if (bytes >= 1024)
        return g_strdup_printf("%.0f kB", bytes / 1024.0);

    return g_strdup_printf("%" G_GINT64_FORMAT " B", bytes);
}

/*
 * Draws the bar in place.
 *
 * A download is the one thing here that takes minutes, and a command that
 * prints nothing for six of them is indistinguishable from one that has
 * hung.
 */
static void
draw_progress(const gchar *name, gint64 done, gint64 total)
{
    g_autofree gchar *done_text = human_bytes(done);
    g_autofree gchar *total_text = NULL;
    gint filled;
    gint i;

    if (total <= 0) {
        g_print("\r  %-28s %s   ", name, done_text);
        return;
    }

    total_text = human_bytes(total);
    filled = (gint)(done * 30 / total);

    g_print("\r  %-28s [", name);

    for (i = 0; i < 30; i++)
        g_print("%s", i < filled ? "#" : "-");

    g_print("] %3" G_GINT64_FORMAT "%%  %s / %s   ",
            done * 100 / total, done_text, total_text);
}

static void
on_image_event(ClawtClient *client, ClawtEvent *event, gpointer user_data)
{
    ImageWatch *watch = user_data;
    const gchar *kind = clawt_event_get_kind(event);

    (void)client;

    if (g_strcmp0(clawt_event_get_subject(event), watch->name) != 0)
        return;

    if (g_strcmp0(kind, "image.progress") == 0) {
        draw_progress(watch->name,
                      clawt_event_get_detail_int(event, "done"),
                      clawt_event_get_detail_int(event, "total"));
        return;
    }

    if (g_strcmp0(kind, "image.finished") == 0) {
        const gchar *failure = clawt_event_get_detail(event, "error");

        g_print("\n");

        if (failure != NULL) {
            g_printerr("clawtilla: %s\n", failure);
            watch->status = EXIT_FAILURE;
        } else {
            g_print("%s is ready: %s\n", watch->name,
                    clawt_event_get_detail(event, "path"));
        }

        g_main_loop_quit(watch->loop);
    }
}

static gint
cmd_image_vm(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    const gchar *verb = (argc > 3) ? argv[3] : "list";
    JsonObject *root;
    guint i;

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    if (g_strcmp0(verb, "list") == 0) {
        JsonArray *images;

        reply = call(client, "image.vm_list", NULL);
        if (reply == NULL)
            return EXIT_FAILURE;

        images = json_object_get_array_member(json_node_get_object(reply),
                                              "images");

        if (json_array_get_length(images) == 0) {
            g_print("No cloud images yet.\n\n");
            g_print("  clawtilla image vm catalog        what is suggested\n");
            g_print("  clawtilla image vm get fedora-44  fetch one\n");
            return EXIT_SUCCESS;
        }

        for (i = 0; i < json_array_get_length(images); i++) {
            JsonObject *image = json_array_get_object_element(images, i);
            g_autofree gchar *size =
                human_bytes(json_object_get_int_member(image, "bytes"));

            if (json_object_get_boolean_member(image, "downloading")) {
                gint64 total = json_object_get_int_member(image, "total");
                g_autofree gchar *expected = human_bytes(total);

                g_print("  %-40s %s of %s (downloading)\n",
                        member_or(image, "name", "?"), size, expected);
                continue;
            }

            g_print("  %-40s %s\n", member_or(image, "name", "?"), size);
        }

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "catalog") == 0) {
        JsonArray *sources;
        const gchar *last_group = NULL;

        reply = call(client, "image.vm_catalog", NULL);
        if (reply == NULL)
            return EXIT_FAILURE;

        sources = json_object_get_array_member(json_node_get_object(reply),
                                               "sources");

        for (i = 0; i < json_array_get_length(sources); i++) {
            JsonObject *source = json_array_get_object_element(sources, i);
            const gchar *group = member_or(source, "group", "Other");

            if (g_strcmp0(group, last_group) != 0) {
                g_print("%s%s\n", (last_group != NULL) ? "\n" : "", group);
                last_group = group;
            }

            g_print("  %-18s %-28s %s\n", member_or(source, "id", "?"),
                    member_or(source, "name", "?"),
                    member_or(source, "note", ""));
        }

        g_print("\nAny https URL works too:\n");
        g_print("  clawtilla image vm get https://example.com/disk.qcow2\n");

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "get") == 0) {
        g_autoptr(JsonBuilder) builder = json_builder_new();
        g_autoptr(GMainContext) context = NULL;
        g_autoptr(GMainLoop) loop = NULL;
        ImageWatch watch;
        const gchar *url = (argc > 4) ? argv[4] : NULL;

        if (url == NULL) {
            g_printerr("Usage: clawtilla image vm get <url-or-id> "
                       "[name]\n");
            g_printerr("  e.g. clawtilla image vm get fedora-44\n");
            return EXIT_FAILURE;
        }

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "url");
        json_builder_add_string_value(builder, url);

        if (argc > 5) {
            json_builder_set_member_name(builder, "name");
            json_builder_add_string_value(builder, argv[5]);
        }

        json_builder_end_object(builder);

        /*
         * Subscribed before asking, because a small image can finish
         * before a subscription made afterwards would have seen anything
         * -- and then this waits for an event that has already been and
         * gone.
         */
        watch.loop = loop = g_main_loop_new(NULL, FALSE);
        watch.status = EXIT_SUCCESS;
        watch.name = NULL;

        g_signal_connect(client, "event", G_CALLBACK(on_image_event),
                         &watch);

        if (!clawt_client_subscribe(client, 0, NULL, NULL)) {
            g_printerr("clawtilla: could not subscribe to the event "
                       "stream\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "image.vm_download",
                     json_builder_get_root(builder));

        if (reply == NULL)
            return EXIT_FAILURE;

        root = json_node_get_object(reply);
        watch.name = member_or(root, "name", url);

        g_print("Fetching %s\n", watch.name);

        context = g_main_context_ref(g_main_context_default());
        (void)context;

        g_main_loop_run(loop);

        return watch.status;
    }

    if (g_strcmp0(verb, "rm") == 0 || g_strcmp0(verb, "cancel") == 0) {
        g_autoptr(JsonBuilder) builder = json_builder_new();
        const gchar *name = (argc > 4) ? argv[4] : NULL;
        gboolean force = (argc > 5) && g_strcmp0(argv[5], "--force") == 0;

        if (name == NULL) {
            g_printerr("Usage: clawtilla image vm %s <name> [--force]\n",
                       verb);
            return EXIT_FAILURE;
        }

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, name);

        if (force) {
            json_builder_set_member_name(builder, "force");
            json_builder_add_boolean_value(builder, TRUE);
        }

        json_builder_end_object(builder);

        reply = call(client,
                     g_strcmp0(verb, "rm") == 0 ? "image.vm_remove"
                                                : "image.vm_cancel",
                     json_builder_get_root(builder));

        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("%s %s\n", name,
                g_strcmp0(verb, "rm") == 0 ? "removed" : "cancelled");

        return EXIT_SUCCESS;
    }

    g_printerr("Usage: clawtilla image vm <list|catalog|get|rm|cancel>\n");
    g_printerr("  clawtilla image vm get fedora-44\n");
    g_printerr("  clawtilla image vm rm fedora-44 --force\n");
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

    if (argc > 2 && g_strcmp0(argv[2], "vm") == 0)
        return cmd_image_vm(argc, argv);

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
    g_print("\nFor VM disk images: clawtilla image vm\n");

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

/*
 * Reads a secret from stdin, or from a terminal without echoing it.
 *
 * A password on a command line is in the shell history and in the
 * process table for as long as the command runs, which is exactly long
 * enough for somebody else on the machine to see it.  There is
 * deliberately no --password flag.
 */
static gchar *
read_secret(const gchar *prompt)
{
    gchar *line = NULL;
    gsize length = 0;

    if (isatty(STDIN_FILENO)) {
        struct termios original;
        struct termios quiet;
        gboolean hushed = FALSE;

        g_printerr("%s", prompt);

        if (tcgetattr(STDIN_FILENO, &original) == 0) {
            quiet = original;
            quiet.c_lflag &= ~(tcflag_t)ECHO;
            hushed = tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet) == 0;
        }

        if (getline(&line, &length, stdin) < 0)
            g_clear_pointer(&line, free);

        if (hushed)
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);

        g_printerr("\n");
    } else {
        if (getline(&line, &length, stdin) < 0)
            g_clear_pointer(&line, free);
    }

    if (line == NULL)
        return NULL;

    g_strchomp(line);

    return line;
}

/*
 * `key=value` arguments, turned into the payload members the daemon
 * expects.
 *
 * Lists are comma-separated -- `rooms=!a:x,!b:x` -- and a secret is
 * `access_token=env:NAME`, never a literal, because there is no way to
 * write a secret's value into clawtilla.yaml and this is not going to be
 * the first one.
 */
static gboolean
apply_setting(JsonBuilder *builder, const gchar *argument)
{
    g_auto(GStrv) parts = g_strsplit(argument, "=", 2);
    g_autofree gchar *schema_key = NULL;
    const ClawtSchemaEntry *entry;
    const gchar *key;
    const gchar *value;

    if (parts[0] == NULL || parts[1] == NULL) {
        g_printerr("clawtilla: '%s' is not key=value\n", argument);
        return FALSE;
    }

    key = parts[0];
    value = parts[1];

    /*
     * The schema decides how to spell it, rather than a list here.
     * There used to be one, and it fell out of step the first time a
     * key was added: the setting was accepted, reported as saved, and
     * silently dropped.
     */
    schema_key = g_strdup_printf("integrations.%s", key);
    entry = clawt_config_schema_lookup(schema_key);

    if (entry == NULL) {
        g_printerr("clawtilla: '%s' is not something an integration holds\n",
                   key);
        return FALSE;
    }

    switch (entry->type) {
    case CLAWT_SCHEMA_SECRET: {
        g_auto(GStrv) ref = g_strsplit(value, ":", 2);

        if (ref[1] == NULL) {
            g_printerr("clawtilla: %s must be a reference -- file:PATH, "
                       "env:NAME or command:\"...\"\n", key);
            return FALSE;
        }

        json_builder_set_member_name(builder, "secret_key");
        json_builder_add_string_value(builder, key);
        json_builder_set_member_name(builder, "secret_backend");
        json_builder_add_string_value(builder, ref[0]);
        json_builder_set_member_name(builder, "secret_locator");
        json_builder_add_string_value(builder, ref[1]);

        return TRUE;
    }

    case CLAWT_SCHEMA_STRING_LIST: {
        g_auto(GStrv) values = NULL;
        guint k;

        json_builder_set_member_name(builder, key);
        json_builder_begin_array(builder);

        if (*value != '\0') {
            values = g_strsplit(value, ",", -1);

            for (k = 0; values[k] != NULL; k++)
                json_builder_add_string_value(builder, g_strstrip(values[k]));
        }

        json_builder_end_array(builder);

        return TRUE;
    }

    case CLAWT_SCHEMA_INT:
        json_builder_set_member_name(builder, key);
        json_builder_add_int_value(builder, g_ascii_strtoll(value, NULL, 10));
        return TRUE;

    case CLAWT_SCHEMA_BOOLEAN:
        json_builder_set_member_name(builder, key);
        json_builder_add_boolean_value(builder,
                                       g_strcmp0(value, "true") == 0 ||
                                       g_strcmp0(value, "yes") == 0 ||
                                       g_strcmp0(value, "1") == 0);
        return TRUE;

    case CLAWT_SCHEMA_MAPPING:
        g_printerr("clawtilla: %s is a mapping; edit it in the config "
                   "file\n", key);
        return FALSE;

    default:
        json_builder_set_member_name(builder, key);
        json_builder_add_string_value(builder, value);
        return TRUE;
    }
}

/*
 * Builds an add/update payload out of `key=value` arguments.
 */
static JsonNode *
build_integration_payload(const gchar *name, const gchar *type_id,
                          const gchar *agent_id,
                          int argc, char *argv[], int first)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    int i;

    json_builder_begin_object(builder);

    if (name != NULL) {
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, name);
    }

    if (type_id != NULL) {
        json_builder_set_member_name(builder, "type");
        json_builder_add_string_value(builder, type_id);
    }

    if (agent_id != NULL) {
        json_builder_set_member_name(builder, "agent");
        json_builder_add_string_value(builder, agent_id);
    }

    for (i = first; i < argc; i++) {
        /* --agent was lifted out of argv and left as a hole. */
        if (argv[i] == NULL)
            continue;

        if (!apply_setting(builder, argv[i]))
            return NULL;
    }

    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

/* @asked_for: see print_usage_text(). */
static void
print_connector_usage(gboolean asked_for)
{
    static const gchar *text =
"Usage: clawtilla connector <command> [options]\n"
"\n"
"A connector is an account clawtilla holds the credential for. Agents get\n"
"its tools; they never get the credential.\n"
"\n"
"Commands:\n"
"  catalog                 services clawtilla knows how to connect\n"
"  list                    connectors you have, and whether they are live\n"
"  registry-refresh        import the open MCP registry now\n"
"  add <name> --provider <id> [options]\n"
"                          add one, and connect it if it can\n"
"  connect <name>          authorize, or authorize again\n"
"  key <name>              paste a token instead (read from stdin)\n"
"  refresh <name>          renew the credential now\n"
"  revoke <name>           forget it here and withdraw it there\n"
"  rm <name>               remove the connector entirely\n"
"\n"
"Options for add:\n"
"  --provider <id>         from `clawtilla connector catalog`\n"
"  --account <label>       which account, in your words (work, personal)\n"
"  --client-id <id>        the OAuth app you registered with the provider\n"
"  --instance <url>        for a service you host yourself\n"
"  --scopes \"<a b>\"        override what to ask for\n"
"  --scope <all|selected|none>   which agents get it (default: none)\n"
"  --agents <a,b>          with --scope selected\n"
"  --command <cmd>         an MCP server of your own\n"
"  --url <url>             an HTTP MCP server of your own\n"
"  --tools <a,b>           narrow the agent to these tools only\n"
"\n"
"Examples:\n"
"  clawtilla connector catalog\n"
"\n"
"  # A personal access token, no OAuth application to register:\n"
"  clawtilla connector add gh --provider github --account work \\\n"
"      --command github-mcp-server --scope all\n"
"  clawtilla connector key gh\n"
"\n"
"  # Or the full device flow, once you have registered an app:\n"
"  clawtilla connector add gl --provider gitlab --client-id abc123 \\\n"
"      --scope selected --agents researcher,scribe\n"
"\n"
"  # Your own GitLab:\n"
"  clawtilla connector add work --provider gitlab \\\n"
"      --instance https://gitlab.example.com --client-id abc123\n"
"\n"
"  clawtilla connector list\n"
"  clawtilla connector revoke gh\n"
"\n"
"  # With connectors.registry_enabled: true\n"
"  clawtilla connector registry-refresh\n";

    print_usage_text(text, asked_for);
}

/*
 * Waits out an authorization, which takes as long as a person takes.
 *
 * The default request timeout is two minutes and a device code is good
 * for fifteen, so this asks for the longer wait explicitly -- otherwise
 * the CLI reports a timeout for a flow that was about to succeed, and
 * the daemon is left holding a credential nobody was told about.
 */
static gint
await_connector_flow(ClawtClient *client, const gchar *flow)
{
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(GError) error = NULL;

    reply = clawt_client_request_full(client, "connector.await",
                                      build_payload("flow", flow, NULL),
                                      900, &error);

    if (reply == NULL) {
        g_printerr("clawtilla: %s\n", error->message);
        return EXIT_FAILURE;
    }

    g_print("Connected.\n");

    return EXIT_SUCCESS;
}

static gint
connector_connect(ClawtClient *client, const gchar *name)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonObject *root;
    const gchar *method;
    const gchar *flow;

    reply = call(client, "connector.begin", build_payload("name", name, NULL));

    if (reply == NULL)
        return EXIT_FAILURE;

    root = json_node_get_object(reply);
    method = member_or(root, "method", "");
    flow = member_or(root, "flow", NULL);

    if (g_strcmp0(method, "device") == 0) {
        const gchar *complete = member_or(root, "verification_uri_complete",
                                          NULL);

        /*
         * The code goes on its own line and unadorned, because the next
         * thing that happens to it is somebody reading it off a screen
         * and typing it into a phone.
         */
        g_print("\n    %s\n\n", member_or(root, "user_code", "?"));
        g_print("Enter that at %s\n",
                member_or(root, "verification_uri", "?"));

        if (complete != NULL)
            g_print("or open %s, which fills it in for you.\n", complete);
    } else {
        g_print("Open this to approve:\n\n    %s\n\n",
                member_or(root, "authorize_url", "?"));
    }

    g_print("\nWaiting...\n");

    return await_connector_flow(client, flow);
}

/*
 * The relay, which is what an agent's .mcp.json names.
 *
 * Nothing here may write to stdout: that is the MCP channel, and a
 * stray line of ours is a protocol error to the client on the other end
 * of it. Everything diagnostic goes to stderr.
 */
static gint
connector_relay(const gchar *name)
{
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(GPtrArray) catalog = NULL;
    g_autoptr(ClawtIntegrationBinding) binding = NULL;
    g_autoptr(ClawtOauthToken) token = NULL;
    g_autoptr(ClawtConnectorPlan) plan = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *catalog_dir = NULL;
    ClawtIntegrationConfig *instance;
    const ClawtIntegrationInfo *info;
    const ClawtConnectorInfo *connector;
    const gchar *token_file;

    if (name == NULL) {
        g_printerr("clawtilla: connector relay needs a name\n");
        return EXIT_FAILURE;
    }

    /*
     * Renewal is asked of the daemon before the credential is read,
     * because the daemon is the only thing that may write one -- two
     * processes refreshing the same token would each invalidate the
     * other's. A daemon that is not there is not fatal: the token on
     * disk may well still be good, and failing here would take away a
     * server that would have worked.
     */
    {
        g_autoptr(ClawtClient) client = connect_to_daemon();

        if (client != NULL) {
            g_autoptr(JsonNode) ignored = NULL;
            g_autoptr(GError) refresh_error = NULL;

            ignored = clawt_client_request(client, "connector.refresh",
                                           build_payload("name", name, NULL),
                                           &refresh_error);
        }
    }

    config = clawt_config_load(opt_config_path, &error);

    if (config == NULL) {
        g_printerr("clawtilla: %s\n", error->message);
        return EXIT_FAILURE;
    }

    instance = clawt_config_get_integration(config, name);

    if (instance == NULL) {
        g_printerr("clawtilla: there is no connector called '%s'\n", name);
        return EXIT_FAILURE;
    }

    info = clawt_integration_find(
        clawt_integration_config_get_type_id(instance));

    if (info == NULL || g_strcmp0(info->id, "connector") != 0) {
        g_printerr("clawtilla: '%s' is not a connector\n", name);
        return EXIT_FAILURE;
    }

    catalog_dir = clawt_config_get_path_value(config, "connectors.dir");
    catalog = clawt_connector_catalog_load(catalog_dir, NULL);
    connector = clawt_connector_catalog_find(
        catalog, clawt_integration_config_get_string(instance, NULL,
                                                     "provider"));

    if (connector == NULL) {
        g_printerr("clawtilla: '%s' names a provider clawtilla does not "
                   "know\n", name);
        return EXIT_FAILURE;
    }

    binding = clawt_integration_binding_for_instance(instance, info, NULL);
    token_file = clawt_integration_binding_get_string(binding, "token_file");

    if (token_file == NULL) {
        g_printerr("clawtilla: '%s' is not connected yet; run `clawtilla "
                   "connector connect %s`\n", name, name);
        return EXIT_FAILURE;
    }

    token = clawt_oauth_token_load(token_file, &error);

    if (token == NULL) {
        g_printerr("clawtilla: cannot read the credential for '%s': %s\n",
                   name, error->message);
        return EXIT_FAILURE;
    }

    plan = clawt_connector_plan_new(connector, binding, token->access_token,
                                    &error);

    if (plan == NULL) {
        g_printerr("clawtilla: %s\n", error->message);
        return EXIT_FAILURE;
    }

    return clawt_connector_relay_run(plan);
}

static gint
cmd_connector(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    const gchar *verb = (argc > 2) ? argv[2] : "list";
    const gchar *name = (argc > 3) ? argv[3] : NULL;
    guint i;

    if (g_strcmp0(verb, "help") == 0 || g_strcmp0(verb, "--help") == 0 ||
        g_strcmp0(verb, "-h") == 0) {
        print_connector_usage(TRUE);
        return EXIT_SUCCESS;
    }

    /*
     * Before connecting to anything: the relay is started by an agent's
     * own CLI and speaks its own protocol on stdout.
     */
    if (g_strcmp0(verb, "relay") == 0)
        return connector_relay(name);

    client = connect_to_daemon();

    if (client == NULL)
        return EXIT_FAILURE;

    if (g_strcmp0(verb, "catalog") == 0) {
        JsonArray *connectors;
        g_autofree gchar *previous = NULL;

        reply = call(client, "connector.catalog", NULL);

        if (reply == NULL)
            return EXIT_FAILURE;

        connectors = json_object_get_array_member(json_node_get_object(reply),
                                                  "connectors");

        for (i = 0; i < json_array_get_length(connectors); i++) {
            JsonObject *entry = json_array_get_object_element(connectors, i);
            const gchar *category = member_or(entry, "category", "");

            if (g_strcmp0(category, previous) != 0) {
                g_print("%s%s\n", i > 0 ? "\n" : "", category);
                g_free(previous);
                previous = g_strdup(category);
            }

            g_print("  %-12s %-8s %s\n", member_or(entry, "id", "?"),
                    member_or(entry, "auth", ""),
                    member_or(entry, "summary", ""));
        }

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "list") == 0) {
        JsonArray *connectors;

        reply = call(client, "connector.list", NULL);

        if (reply == NULL)
            return EXIT_FAILURE;

        connectors = json_object_get_array_member(json_node_get_object(reply),
                                                  "connectors");

        if (json_array_get_length(connectors) == 0) {
            g_print("No connectors yet. `clawtilla connector catalog` lists "
                    "what can be connected.\n");
            return EXIT_SUCCESS;
        }

        for (i = 0; i < json_array_get_length(connectors); i++) {
            JsonObject *entry = json_array_get_object_element(connectors, i);
            gboolean connected =
                json_object_get_boolean_member_with_default(entry,
                                                            "connected",
                                                            FALSE);
            gint64 expires =
                json_object_get_int_member_with_default(entry, "expires_at",
                                                        0);
            g_autofree gchar *status = NULL;

            if (!connected) {
                status = g_strdup("not connected");
            } else if (expires == 0) {
                status = g_strdup("connected");
            } else {
                gint64 left = expires - (g_get_real_time() / G_USEC_PER_SEC);

                /*
                 * An expired-but-renewable credential is not a problem
                 * and must not read like one, or somebody learns to
                 * ignore this column.
                 */
                if (left > 0)
                    status = g_strdup_printf("connected, %" G_GINT64_FORMAT
                                             "m left", left / 60);
                else if (json_object_get_boolean_member_with_default(
                             entry, "renewable", FALSE))
                    status = g_strdup("connected, renewing");
                else
                    status = g_strdup("expired -- connect again");
            }

            g_print("%-14s %-10s %-10s %s\n", member_or(entry, "name", "?"),
                    member_or(entry, "provider", "?"),
                    member_or(entry, "scope", ""), status);
        }

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "registry-refresh") == 0) {
        JsonObject *object;
        gint64 imported;

        reply = call(client, "connector.registry_refresh", NULL);

        if (reply == NULL)
            return EXIT_FAILURE;

        object = json_node_get_object(reply);
        imported = json_object_get_int_member_with_default(object,
                                                            "imported", 0);

        g_print("Imported %" G_GINT64_FORMAT " connector%s from the "
                "registry.\n", imported, imported == 1 ? "" : "s");

        return EXIT_SUCCESS;
    }

    /*
     * Which command it is comes before whether it has a name, or a verb
     * nobody implements is answered with "connector bogus needs a name"
     * -- an instruction to add an argument to a command that does not
     * exist.  A refusal that names the wrong problem sends somebody a
     * long way off.
     */
    if (g_strcmp0(verb, "add") != 0 && g_strcmp0(verb, "connect") != 0 &&
        g_strcmp0(verb, "key") != 0 && g_strcmp0(verb, "refresh") != 0 &&
        g_strcmp0(verb, "revoke") != 0 && g_strcmp0(verb, "rm") != 0) {
        g_printerr("clawtilla: unknown connector command '%s'\n", verb);
        print_connector_usage(FALSE);
        return EXIT_FAILURE;
    }

    if (name == NULL) {
        g_printerr("clawtilla: connector %s needs a name\n", verb);
        return EXIT_FAILURE;
    }

    if (g_strcmp0(verb, "add") == 0) {
        g_autoptr(JsonBuilder) builder = json_builder_new();
        const gchar *client_id = NULL;
        gboolean have_provider = FALSE;
        int a;

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, name);
        json_builder_set_member_name(builder, "type");
        json_builder_add_string_value(builder, "connector");

        /*
         * Dashes fold to underscores, because the member name the
         * daemon looks for is the schema key: a `--client-id` passed
         * straight through becomes `client-id`, which nothing has ever
         * heard of and which is ignored without a word.
         */
        for (a = 4; a + 1 < argc; a += 2) {
            g_autofree gchar *key = NULL;

            if (argv[a] == NULL || !g_str_has_prefix(argv[a], "--"))
                continue;

            key = g_strdup(argv[a] + 2);
            g_strdelimit(key, "-", '_');

            if (g_strcmp0(key, "agents") == 0 ||
                g_strcmp0(key, "tools") == 0) {
                g_auto(GStrv) values = g_strsplit(argv[a + 1], ",", -1);
                gsize v;

                json_builder_set_member_name(builder, key);
                json_builder_begin_array(builder);

                for (v = 0; values[v] != NULL; v++)
                    json_builder_add_string_value(builder,
                                                  g_strstrip(values[v]));

                json_builder_end_array(builder);
                continue;
            }

            if (g_strcmp0(key, "provider") == 0)
                have_provider = TRUE;

            if (g_strcmp0(key, "client_id") == 0)
                client_id = argv[a + 1];

            json_builder_set_member_name(builder, key);
            json_builder_add_string_value(builder, argv[a + 1]);
        }

        json_builder_end_object(builder);

        if (!have_provider) {
            g_printerr("clawtilla: connector add needs --provider; "
                       "`clawtilla connector catalog` lists them\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "integration.add",
                     json_builder_get_root(builder));

        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("Added connector '%s'.\n", name);

        /*
         * Connecting straight away only when there is an application to
         * connect with. Without a client id the provider has nothing to
         * identify the request, and starting a flow that cannot succeed
         * is worse than saying what is missing.
         */
        if (client_id != NULL)
            return connector_connect(client, name);

        g_print("\nNow give it a credential, either:\n"
                "  clawtilla connector key %s        (paste a token)\n"
                "  clawtilla connector connect %s    (needs --client-id)\n",
                name, name);

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "connect") == 0)
        return connector_connect(client, name);

    if (g_strcmp0(verb, "key") == 0) {
        g_autofree gchar *key = NULL;

        /*
         * Read from stdin with the echo off, and deliberately no
         * --key flag: an argument is in the shell history and in the
         * process table for anybody on the machine to read.
         */
        key = read_secret("Token: ");

        if (key == NULL || *key == '\0') {
            g_printerr("clawtilla: no token was given\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "connector.key",
                     build_payload("name", name, "key", key, NULL));

        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("Stored in %s\n",
                member_or(json_node_get_object(reply), "token_file", "?"));

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "refresh") == 0) {
        reply = call(client, "connector.refresh",
                     build_payload("name", name, NULL));

        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("Renewed.\n");

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "revoke") == 0) {
        JsonObject *root;

        reply = call(client, "connector.revoke",
                     build_payload("name", name, NULL));

        if (reply == NULL)
            return EXIT_FAILURE;

        root = json_node_get_object(reply);

        if (json_object_get_boolean_member_with_default(root, "told_provider",
                                                        FALSE)) {
            g_print("Revoked, and the provider was told.\n");
        } else {
            const gchar *note = member_or(root, "note", NULL);

            g_print("Forgotten here.\n");

            if (note != NULL)
                g_print("%s\n", note);
        }

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "rm") == 0) {
        /*
         * The credential goes first. Removing the integration and
         * leaving the token file would strand a live credential under a
         * name nothing refers to any more.
         */
        g_autoptr(JsonNode) revoked = NULL;
        g_autoptr(GError) ignored = NULL;

        revoked = clawt_client_request(client, "connector.revoke",
                                       build_payload("name", name, NULL),
                                       &ignored);

        reply = call(client, "integration.remove",
                     build_payload("name", name, NULL));

        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("Removed connector '%s'.\n", name);

        return EXIT_SUCCESS;
    }

    /*
     * Only reachable for one of the six name-taking commands above
     * losing its branch; an unknown one was refused before the name was
     * demanded.  Same shape as cmd_agent()'s tail, and the same reason.
     */
    g_printerr("clawtilla: 'connector %s' is listed but not implemented\n",
               verb);

    return EXIT_FAILURE;
}

/* @asked_for: see print_usage_text(). */
static void
print_integration_usage(gboolean asked_for)
{
    static const gchar *text =
        "Usage: clawtilla integration <verb> [...]\n"
        "\n"
        "  types                          what kinds there are\n"
        "  list [agent]                   the instances, and what an agent has\n"
        "  show <name>                    one instance in full\n"
        "  add <name> <type> [key=value]  add one\n"
        "  set <name> [key=value ...]     change one\n"
        "  set <name> --agent <id> ...    change it for one agent only\n"
        "  scope <name> all|none|<ids>    who gets it\n"
        "  rm <name>                      remove it\n"
        "  health <agent> [name]          can it reach what it talks to\n"
        "  test <name>                    send one notification now\n"
        "  matrix-login <name> <homeserver> <user>\n"
        "                                 sign in; the password is read from\n"
        "                                 stdin and never stored\n"
        "  matrix-rooms <name>            the rooms that account is in\n"
        "\n"
        "Examples:\n"
        "  clawtilla integration add home matrix \\\n"
        "      homeserver=https://matrix.example.org\n"
        "  clawtilla integration matrix-login home https://matrix.example.org "
        "agent\n"
        "  clawtilla integration scope home researcher,scribe\n"
        "  clawtilla integration add github mcp scope=all \\\n"
        "      command=npx args=-y,@modelcontextprotocol/server-github\n"
        "  clawtilla integration set home --agent researcher \\\n"
        "      user_id=@researcher:example.org\n"
        "  clawtilla integration add phone notify scope=all \\\n"
        "      backend=ntfy url=https://ntfy.sh/my-topic events=question,error\n"
        "  clawtilla integration add desk notify scope=all backend=command \\\n"
        "      command=receipt-print\n"
        "  clawtilla integration test phone\n";

    print_usage_text(text, asked_for);
}

static void
print_integration_row(JsonObject *integration)
{
    JsonArray *effective =
        json_object_has_member(integration, "effective_agents")
            ? json_object_get_array_member(integration, "effective_agents")
            : NULL;
    const gchar *scope = member_or(integration, "scope", "?");
    g_autofree gchar *reach = NULL;

    if (g_strcmp0(scope, "all") == 0) {
        reach = g_strdup_printf("all (%u)",
                                effective != NULL
                                    ? json_array_get_length(effective) : 0);
    } else if (effective == NULL || json_array_get_length(effective) == 0) {
        reach = g_strdup("nobody");
    } else {
        GString *names = g_string_new(NULL);
        guint i;

        for (i = 0; i < json_array_get_length(effective); i++) {
            if (i > 0)
                g_string_append(names, ",");

            g_string_append(names, json_array_get_string_element(effective, i));
        }

        reach = g_string_free(names, FALSE);
    }

    g_print("%-16s %-8s %-4s %s\n",
            member_or(integration, "name", "?"),
            member_or(integration, "type", "?"),
            json_object_get_boolean_member(integration, "enabled")
                ? "on" : "off",
            reach);

    if (json_object_has_member(integration, "shadow_reason"))
        g_print("                 disabled: %s\n",
                member_or(integration, "shadow_reason", ""));
}

static gint
cmd_integration(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    const gchar *verb = (argc > 2) ? argv[2] : "list";
    const gchar *name = (argc > 3) ? argv[3] : NULL;
    const gchar *agent_id = NULL;
    guint i;
    int settings_start = 4;

    /*
     * --agent is pulled out here rather than left as a key=value, because
     * it selects *where* the other settings are written rather than being
     * one of them.
     */
    {
        int a;

        for (a = 3; a + 1 < argc; a++) {
            if (g_strcmp0(argv[a], "--agent") != 0)
                continue;

            agent_id = argv[a + 1];
            argv[a] = NULL;
            argv[a + 1] = NULL;
            break;
        }
    }

    if (g_strcmp0(verb, "help") == 0 || g_strcmp0(verb, "--help") == 0 ||
        g_strcmp0(verb, "-h") == 0) {
        print_integration_usage(TRUE);
        return EXIT_SUCCESS;
    }

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    if (g_strcmp0(verb, "types") == 0) {
        JsonArray *types;

        reply = call(client, "integration.types", NULL);
        if (reply == NULL)
            return EXIT_FAILURE;

        types = json_object_get_array_member(json_node_get_object(reply),
                                             "types");

        for (i = 0; i < json_array_get_length(types); i++) {
            JsonObject *type = json_array_get_object_element(types, i);

            g_print("%-8s %-8s %s\n", member_or(type, "id", "?"),
                    member_or(type, "kind", ""),
                    member_or(type, "summary", ""));
        }

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "list") == 0) {
        JsonObject *root;
        JsonArray *integrations;
        JsonArray *warnings;

        reply = call(client, "integration.list",
                     build_payload("agent", name, NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        root = json_node_get_object(reply);
        integrations = json_object_get_array_member(root, "integrations");

        if (json_array_get_length(integrations) == 0)
            g_print("No shared integrations. `clawtilla integration add` "
                    "makes one.\n");

        for (i = 0; i < json_array_get_length(integrations); i++)
            print_integration_row(
                json_array_get_object_element(integrations, i));

        /*
         * An agent's own inline blocks are not instances and are invisible
         * in the list above, so an agent with a Matrix block of its own
         * would otherwise look like it had nothing.
         */
        if (json_object_has_member(root, "bindings")) {
            JsonArray *bindings = json_object_get_array_member(root,
                                                               "bindings");

            g_print("\n%s has:\n", name);

            if (json_array_get_length(bindings) == 0)
                g_print("  nothing\n");

            for (i = 0; i < json_array_get_length(bindings); i++) {
                JsonObject *binding =
                    json_array_get_object_element(bindings, i);
                gboolean valid =
                    json_object_get_boolean_member(binding, "valid");

                g_print("  %-16s %-8s %-8s %s\n",
                        member_or(binding, "name", "?"),
                        member_or(binding, "type", "?"),
                        json_object_get_boolean_member(binding, "shared")
                            ? "shared" : "its own",
                        valid ? "" : member_or(binding, "problem", ""));
            }
        }

        warnings = json_object_get_array_member(root, "warnings");

        for (i = 0; i < json_array_get_length(warnings); i++)
            g_printerr("warning: %s\n",
                       json_array_get_string_element(warnings, i));

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "show") == 0) {
        JsonArray *integrations;

        if (name == NULL) {
            g_printerr("Usage: clawtilla integration show <name>\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "integration.list", NULL);
        if (reply == NULL)
            return EXIT_FAILURE;

        integrations = json_object_get_array_member(
            json_node_get_object(reply), "integrations");

        for (i = 0; i < json_array_get_length(integrations); i++) {
            JsonObject *integration =
                json_array_get_object_element(integrations, i);
            g_autoptr(GList) members = NULL;
            GList *l;

            if (g_strcmp0(member_or(integration, "name", ""), name) != 0)
                continue;

            members = json_object_get_members(integration);

            for (l = members; l != NULL; l = l->next) {
                g_autoptr(JsonGenerator) generator = json_generator_new();
                g_autofree gchar *text = NULL;
                JsonNode *value = json_object_get_member(integration,
                                                         l->data);

                json_generator_set_root(generator, value);
                text = json_generator_to_data(generator, NULL);
                g_print("%-18s %s\n", (const gchar *)l->data, text);
            }

            return EXIT_SUCCESS;
        }

        g_printerr("clawtilla: there is no integration called '%s'\n", name);
        return EXIT_FAILURE;
    }

    if (g_strcmp0(verb, "add") == 0) {
        g_autoptr(JsonNode) payload = NULL;
        const gchar *type_id = (argc > 4) ? argv[4] : NULL;

        if (name == NULL || type_id == NULL) {
            g_printerr("Usage: clawtilla integration add <name> <type> "
                       "[key=value ...]\n");
            return EXIT_FAILURE;
        }

        payload = build_integration_payload(name, type_id, agent_id, argc,
                                            argv, 5);
        if (payload == NULL)
            return EXIT_FAILURE;

        reply = call(client, "integration.add", g_steal_pointer(&payload));
        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("Added %s.\n", name);
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "set") == 0) {
        g_autoptr(JsonNode) payload = NULL;

        if (name == NULL) {
            g_printerr("Usage: clawtilla integration set <name> "
                       "[--agent <id>] key=value ...\n");
            return EXIT_FAILURE;
        }

        payload = build_integration_payload(name, NULL, agent_id, argc, argv,
                                            settings_start);
        if (payload == NULL)
            return EXIT_FAILURE;

        reply = call(client, "integration.update", g_steal_pointer(&payload));
        return reply != NULL ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (g_strcmp0(verb, "scope") == 0) {
        g_autoptr(JsonBuilder) builder = json_builder_new();
        const gchar *scope = (argc > 4) ? argv[4] : NULL;

        if (name == NULL || scope == NULL) {
            g_printerr("Usage: clawtilla integration scope <name> "
                       "all|none|<agent,agent>\n");
            return EXIT_FAILURE;
        }

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, name);

        if (g_strcmp0(scope, "all") == 0 || g_strcmp0(scope, "none") == 0) {
            json_builder_set_member_name(builder, "scope");
            json_builder_add_string_value(builder, scope);
        } else {
            g_auto(GStrv) ids = g_strsplit(scope, ",", -1);
            guint k;

            json_builder_set_member_name(builder, "scope");
            json_builder_add_string_value(builder, "selected");
            json_builder_set_member_name(builder, "agents");
            json_builder_begin_array(builder);

            for (k = 0; ids[k] != NULL; k++)
                json_builder_add_string_value(builder, g_strstrip(ids[k]));

            json_builder_end_array(builder);
        }

        json_builder_end_object(builder);

        reply = call(client, "integration.update",
                     json_builder_get_root(builder));
        return reply != NULL ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (g_strcmp0(verb, "rm") == 0) {
        if (name == NULL) {
            g_printerr("Usage: clawtilla integration rm <name>\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "integration.remove",
                     build_payload("name", name, NULL));

        if (reply == NULL)
            return EXIT_FAILURE;

        /*
         * Said out loud, because it is the surprising half: the token
         * file stays where it is, so removing an integration by mistake
         * costs a retype of the config and not another sign-in.
         */
        g_print("Removed %s. Any credential file it wrote is still on "
                "disk.\n", name);
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "test") == 0) {
        if (name == NULL) {
            g_printerr("Usage: clawtilla integration test <name>\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "integration.notify_test",
                     build_payload("integration", name, NULL));

        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("Sent. If nothing arrived, the notifier is not reaching "
                "you.\n");
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "matrix-login") == 0) {
        const gchar *homeserver = (argc > 4) ? argv[4] : NULL;
        const gchar *user = (argc > 5) ? argv[5] : NULL;
        g_autofree gchar *password = NULL;
        JsonObject *root;

        if (name == NULL || homeserver == NULL || user == NULL) {
            g_printerr("Usage: clawtilla integration matrix-login <name> "
                       "<homeserver> <user> [--agent <id>]\n");
            return EXIT_FAILURE;
        }

        password = read_secret("Matrix password: ");

        if (password == NULL || *password == '\0') {
            g_printerr("clawtilla: no password given\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "integration.matrix_login",
                     build_payload("integration", name,
                                   "homeserver", homeserver,
                                   "user", user,
                                   "password", password,
                                   "agent", agent_id, NULL));

        /* Gone from this process as soon as it is on its way. */
        memset(password, 0, strlen(password));

        if (reply == NULL)
            return EXIT_FAILURE;

        root = json_node_get_object(reply);

        g_print("Signed in as %s.\n", member_or(root, "user_id", "?"));
        g_print("The token is in %s, and the config now refers to it.\n",
                member_or(root, "token_file", "?"));
        g_print("It appears on your account's device list as \"clawtilla\"; "
                "sign that device out to revoke it.\n");

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "matrix-rooms") == 0) {
        JsonArray *rooms;

        if (name == NULL) {
            g_printerr("Usage: clawtilla integration matrix-rooms <name> "
                       "[--agent <id>]\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "integration.matrix_rooms",
                     build_payload("integration", name, "agent", agent_id,
                                   NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        rooms = json_object_get_array_member(json_node_get_object(reply),
                                             "rooms");

        if (json_array_get_length(rooms) == 0)
            g_print("That account is not in any rooms yet.\n");

        for (i = 0; i < json_array_get_length(rooms); i++) {
            JsonObject *room = json_array_get_object_element(rooms, i);

            g_print("%-40s %s\n", member_or(room, "id", "?"),
                    member_or(room, "label", ""));
        }

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "health") == 0) {
        JsonArray *checks;
        gint status = EXIT_SUCCESS;

        if (name == NULL) {
            g_printerr("Usage: clawtilla integration health <agent> "
                       "[integration]\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "integration.health",
                     build_payload("agent", name, "integration",
                                   argc > 4 ? argv[4] : NULL, NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        checks = json_object_get_array_member(json_node_get_object(reply),
                                              "checks");

        if (json_array_get_length(checks) == 0) {
            g_print("%s has no integrations.\n", name);
            return EXIT_SUCCESS;
        }

        for (i = 0; i < json_array_get_length(checks); i++) {
            JsonObject *check = json_array_get_object_element(checks, i);
            gboolean ok = json_object_get_boolean_member(check, "ok");

            g_print("%-16s %-8s %s%s%s\n", member_or(check, "id", "?"),
                    member_or(check, "type", ""),
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
    print_integration_usage(FALSE);
    return EXIT_FAILURE;
}

/* @asked_for: see print_usage_text(). */
static void
print_skill_usage(gboolean asked_for)
{
    static const gchar *text =
        "Usage: clawtilla skill <verb> [...]\n"
        "\n"
        "  list                       every skill, and whether it is on\n"
        "  show <name>                one in full, with its provenance\n"
        "  new <name> <description>   write a new one and open it\n"
        "  import <dir> [origin]      copy one in, disabled, markdown only\n"
        "  edit <name>                open its SKILL.md in $EDITOR\n"
        "  rm <name>                  delete it\n"
        "  enable <name>              let it reach the agents that have it\n"
        "  disable <name>             stop it reaching them\n"
        "  assign <name> <target>     to an agent, team:<id>, or fleet\n"
        "  unassign <name> <target>   take it away again\n"
        "  reload                     rescan the library now\n"
        "\n"
        "An imported skill lands disabled and its scripts are left behind.\n"
        "Read it, then enable it: what a skill says goes into a model's\n"
        "context with your agent's own authority.\n"
        "\n"
        "Examples:\n"
        "  clawtilla skill import ~/src/some-repo/skills/release\n"
        "  clawtilla skill show release\n"
        "  clawtilla skill enable release\n"
        "  clawtilla skill assign release team:engineering\n";

    print_usage_text(text, asked_for);
}

/*
 * Print a skill's warnings and skipped files.
 *
 * Shown in `list` as a count and in `show` in full, because the whole
 * point of the scan is that a person reads it before enabling anything
 * -- a warning that only appears if you go looking is one that gets
 * enabled around.
 */
static void
print_skill_notes(JsonObject *skill)
{
    JsonArray *warnings = json_object_has_member(skill, "warnings")
                          ? json_object_get_array_member(skill, "warnings")
                          : NULL;
    JsonArray *skipped = json_object_has_member(skill, "skipped")
                         ? json_object_get_array_member(skill, "skipped")
                         : NULL;
    guint i;

    for (i = 0; warnings != NULL && i < json_array_get_length(warnings); i++)
        g_print("  ! %s\n", json_array_get_string_element(warnings, i));

    if (skipped != NULL && json_array_get_length(skipped) > 0) {
        g_print("  Not copied (markdown only):");

        for (i = 0; i < json_array_get_length(skipped); i++)
            g_print(" %s", json_array_get_string_element(skipped, i));

        g_print("\n");
    }
}

static gint
cmd_skill(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    const gchar *verb = (argc > 2) ? argv[2] : "list";
    const gchar *name = (argc > 3) ? argv[3] : NULL;
    guint i;

    if (g_strcmp0(verb, "help") == 0 || g_strcmp0(verb, "--help") == 0 ||
        g_strcmp0(verb, "-h") == 0) {
        print_skill_usage(TRUE);
        return EXIT_SUCCESS;
    }

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    if (g_strcmp0(verb, "list") == 0) {
        JsonObject *object;
        JsonArray *skills;
        JsonArray *problems;

        reply = call(client, "skill.list", NULL);
        if (reply == NULL)
            return EXIT_FAILURE;

        object = json_node_get_object(reply);
        skills = json_object_get_array_member(object, "skills");
        problems = json_object_get_array_member(object, "problems");

        /*
         * An empty list has two causes and they send you to different
         * places, so it says which rather than printing nothing.
         */
        if (json_array_get_length(skills) == 0) {
            if (!json_object_get_boolean_member(object, "enabled"))
                g_print("Skills are turned off for this fleet "
                        "(skills.enabled).\n");
            else
                g_print("No skills in %s. `clawtilla skill new` writes one, "
                        "`clawtilla skill import` copies one in.\n",
                        member_or(object, "directory", "the skills "
                                                       "directory"));
        } else {
            g_print("%-24s %-4s %-9s %s\n", "NAME", "ON", "SOURCE",
                    "DESCRIPTION");

            for (i = 0; i < json_array_get_length(skills); i++) {
                JsonObject *skill = json_array_get_object_element(skills, i);
                JsonArray *warnings =
                    json_object_get_array_member(skill, "warnings");
                guint n = json_array_get_length(warnings);

                g_print("%-24s %-4s %-9s %s%s\n",
                        member_or(skill, "name", "?"),
                        json_object_get_boolean_member(skill, "enabled")
                            ? "yes" : "no",
                        member_or(skill, "source", "?"),
                        member_or(skill, "description", ""),
                        n > 0 ? "" : "");

                if (n > 0)
                    g_print("  ! %u warning%s -- `clawtilla skill show %s`\n",
                            n, n == 1 ? "" : "s",
                            member_or(skill, "name", "?"));
            }
        }

        for (i = 0; i < json_array_get_length(problems); i++)
            g_printerr("clawtilla: %s\n",
                       json_array_get_string_element(problems, i));

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "show") == 0 || g_strcmp0(verb, "edit") == 0) {
        g_autoptr(JsonBuilder) builder = json_builder_new();
        JsonObject *skill;

        if (name == NULL) {
            g_printerr("Usage: clawtilla skill %s <name>\n", verb);
            return EXIT_FAILURE;
        }

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, name);
        json_builder_end_object(builder);

        reply = call(client, "skill.show", json_builder_get_root(builder));
        if (reply == NULL)
            return EXIT_FAILURE;

        skill = json_node_get_object(reply);

        if (g_strcmp0(verb, "edit") == 0) {
            g_autofree gchar *path =
                g_build_filename(member_or(skill, "directory", "."),
                                 "SKILL.md", NULL);

            {
                g_autoptr(GPtrArray) paths = g_ptr_array_new();

                g_ptr_array_add(paths, path);

                return run_editor(paths);
            }
        }

        g_print("%s\n", member_or(skill, "name", "?"));
        g_print("  %s\n", member_or(skill, "description", ""));
        g_print("  enabled: %s\n",
                json_object_get_boolean_member(skill, "enabled")
                    ? "yes"
                    : "no -- nothing in it reaches an agent");
        g_print("  source:  %s\n", member_or(skill, "source", "?"));

        if (member_or(skill, "origin", NULL) != NULL)
            g_print("  from:    %s\n", member_or(skill, "origin", ""));

        if (member_or(skill, "sha256", NULL) != NULL)
            g_print("  sha256:  %s\n", member_or(skill, "sha256", ""));

        g_print("  at:      %s\n", member_or(skill, "directory", "?"));
        print_skill_notes(skill);
        g_print("\n%s\n", member_or(skill, "body", ""));

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "new") == 0) {
        g_autoptr(JsonBuilder) builder = json_builder_new();

        if (name == NULL || argc < 5) {
            g_printerr("Usage: clawtilla skill new <name> <description>\n");
            return EXIT_FAILURE;
        }

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, name);
        json_builder_set_member_name(builder, "description");
        json_builder_add_string_value(builder, argv[4]);
        json_builder_end_object(builder);

        reply = call(client, "skill.create", json_builder_get_root(builder));
        if (reply == NULL)
            return EXIT_FAILURE;

        {
            JsonObject *skill = json_node_get_object(reply);
            g_autofree gchar *path =
                g_build_filename(member_or(skill, "directory", "."),
                                 "SKILL.md", NULL);

            g_print("Wrote %s\n", path);

            {
                g_autoptr(GPtrArray) paths = g_ptr_array_new();

                g_ptr_array_add(paths, path);

                return run_editor(paths);
            }
        }
    }

    if (g_strcmp0(verb, "import") == 0) {
        g_autoptr(JsonBuilder) builder = json_builder_new();
        JsonObject *skill;

        if (name == NULL) {
            g_printerr("Usage: clawtilla skill import <dir> [origin]\n");
            return EXIT_FAILURE;
        }

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "source");
        json_builder_add_string_value(builder, name);

        if (argc > 4) {
            json_builder_set_member_name(builder, "origin");
            json_builder_add_string_value(builder, argv[4]);
        }

        json_builder_end_object(builder);

        reply = call(client, "skill.import", json_builder_get_root(builder));
        if (reply == NULL)
            return EXIT_FAILURE;

        skill = json_node_get_object(reply);

        g_print("Imported %s, disabled.\n", member_or(skill, "name", "?"));
        print_skill_notes(skill);
        g_print("Read it (`clawtilla skill show %s`), then "
                "`clawtilla skill enable %s`.\n",
                member_or(skill, "name", "?"), member_or(skill, "name", "?"));

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "enable") == 0 || g_strcmp0(verb, "disable") == 0) {
        g_autoptr(JsonBuilder) builder = json_builder_new();

        if (name == NULL) {
            g_printerr("Usage: clawtilla skill %s <name>\n", verb);
            return EXIT_FAILURE;
        }

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, name);
        json_builder_set_member_name(builder, "enabled");
        json_builder_add_boolean_value(builder,
                                       g_strcmp0(verb, "enable") == 0);
        json_builder_end_object(builder);

        reply = call(client, "skill.enable", json_builder_get_root(builder));
        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("%s is now %s.\n", name,
                g_strcmp0(verb, "enable") == 0 ? "enabled" : "disabled");
        report_refusals(reply);

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "rm") == 0) {
        g_autoptr(JsonBuilder) builder = json_builder_new();

        if (name == NULL) {
            g_printerr("Usage: clawtilla skill rm <name>\n");
            return EXIT_FAILURE;
        }

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, name);
        json_builder_end_object(builder);

        reply = call(client, "skill.remove", json_builder_get_root(builder));
        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("Removed %s.\n", name);
        report_refusals(reply);

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "assign") == 0 || g_strcmp0(verb, "unassign") == 0) {
        g_autoptr(JsonBuilder) builder = json_builder_new();
        const gchar *target = (argc > 4) ? argv[4] : NULL;

        if (name == NULL || target == NULL) {
            g_printerr("Usage: clawtilla skill %s <name> "
                       "<agent|team:<id>|fleet>\n", verb);
            return EXIT_FAILURE;
        }

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, name);

        /*
         * `team:` rather than a flag, because an agent id and a team id
         * are both bare words and a client guessing which one somebody
         * meant would sometimes assign to the wrong thing silently.
         */
        if (g_str_has_prefix(target, "team:")) {
            json_builder_set_member_name(builder, "team");
            json_builder_add_string_value(builder,
                                          target + strlen("team:"));
        } else if (g_strcmp0(target, "fleet") == 0) {
            json_builder_set_member_name(builder, "fleet");
            json_builder_add_boolean_value(builder, TRUE);
        } else {
            json_builder_set_member_name(builder, "agent");
            json_builder_add_string_value(builder, target);
        }

        json_builder_end_object(builder);

        reply = call(client,
                     g_strcmp0(verb, "assign") == 0
                         ? "skill.assign" : "skill.unassign",
                     json_builder_get_root(builder));
        if (reply == NULL)
            return EXIT_FAILURE;

        if (!json_object_get_boolean_member(json_node_get_object(reply),
                                            "changed"))
            g_print("No change: %s was already %s %s.\n", name,
                    g_strcmp0(verb, "assign") == 0 ? "assigned to"
                                                   : "absent from",
                    target);
        else
            g_print("%s %s %s.\n", name,
                    g_strcmp0(verb, "assign") == 0 ? "assigned to"
                                                   : "taken from",
                    target);

        report_refusals(reply);

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "reload") == 0) {
        reply = call(client, "skill.reload", NULL);
        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("%" G_GINT64_FORMAT " skill(s) in the library.\n",
                json_object_get_int_member(json_node_get_object(reply),
                                           "count"));
        report_refusals(reply);

        return EXIT_SUCCESS;
    }

    g_printerr("clawtilla: unknown skill verb '%s'\n", verb);
    print_skill_usage(FALSE);
    return EXIT_FAILURE;
}

/* @asked_for: see print_usage_text(). */
static void
print_teach_usage(gboolean asked_for)
{
    g_autoptr(GString) text = g_string_new(
        "Usage: clawtilla teach <verb> [...]\n"
        "\n"
        "  list                          recordings, and what is running\n"
        "  start <agent> [kind] [goal]   watch a task being done\n"
        "  stop <id>                     end the recording\n"
        "  show <id>                     the steps it captured\n"
        "  rm <id>                       delete it and its frames\n"
        "  draft <id>                    write a SKILL.md from it\n"
        "  commit <id>                   add that draft to the library\n"
        "\n"
        "Kinds:\n");
    guint i;

    /*
     * Walked, not written down.  A fourth recorder added to the library
     * has to reach this list without anybody remembering to edit it --
     * the rule this tree applies to every other set of values a person
     * chooses between.
     */
    for (i = 0; i < clawt_teach_source_count(); i++)
        g_string_append_printf(text, "  %-12s %s\n",
                               clawt_teach_source_nth_nick(i),
                               clawt_teach_source_nth_label(i));

    g_string_append(
        text,
        "\n"
        "A demonstration records every key you press, in any window. Read\n"
        "the trace before you turn it into a skill, and read the caveat it\n"
        "carries: on this machine's own desktop a password typed into a\n"
        "window the deny list does not name IS captured.\n"
        "\n"
        "A drafted skill lands disabled and gets the same checks an\n"
        "imported one does.\n"
        "\n"
        "Examples:\n"
        "  clawtilla teach start builder agent \"cut a release\"\n"
        "  clawtilla teach stop 4f2c...\n"
        "  clawtilla teach draft 4f2c...\n"
        "  clawtilla teach commit 4f2c...\n");

    print_usage_text(text->str, asked_for);
}

/*
 * The caveats, printed above everything else.
 *
 * Above, because they are the reason somebody is reading the trace at
 * all -- a caveat under a hundred steps is a caveat scrolled past.
 */
static void
print_teach_caveats(JsonObject *trace)
{
    JsonArray *caveats = json_object_has_member(trace, "caveats")
                         ? json_object_get_array_member(trace, "caveats")
                         : NULL;
    guint i;

    for (i = 0; caveats != NULL && i < json_array_get_length(caveats); i++)
        g_print("! %s\n\n", json_array_get_string_element(caveats, i));
}

static gint
cmd_teach(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    const gchar *verb = (argc > 2) ? argv[2] : "list";
    const gchar *id = (argc > 3) ? argv[3] : NULL;
    guint i;

    if (g_strcmp0(verb, "help") == 0 || g_strcmp0(verb, "--help") == 0 ||
        g_strcmp0(verb, "-h") == 0) {
        print_teach_usage(TRUE);
        return EXIT_SUCCESS;
    }

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    if (g_strcmp0(verb, "list") == 0) {
        JsonArray *recordings;

        reply = call(client, "teach.list", NULL);
        if (reply == NULL)
            return EXIT_FAILURE;

        recordings = json_object_get_array_member(json_node_get_object(reply),
                                                  "recordings");

        if (json_array_get_length(recordings) == 0) {
            g_print("No recordings. `clawtilla teach start <agent>` makes "
                    "one.\n");
            return EXIT_SUCCESS;
        }

        g_print("%-34s %-11s %-12s %-7s %s\n", "ID", "KIND", "AGENT",
                "STEPS", "STATE");

        for (i = 0; i < json_array_get_length(recordings); i++) {
            JsonObject *trace = json_array_get_object_element(recordings, i);

            g_print("%-34s %-11s %-12s %-7" G_GINT64_FORMAT " %s\n",
                    member_or(trace, "id", "?"),
                    member_or(trace, "source", "?"),
                    member_or(trace, "agent", "-"),
                    json_object_get_int_member(trace, "step_count"),
                    json_object_get_boolean_member(trace, "active")
                        ? "recording" : "stopped");
        }

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "start") == 0) {
        const gchar *agent = (argc > 3) ? argv[3] : NULL;
        const gchar *kind = (argc > 4) ? argv[4] : "agent";
        const gchar *goal = (argc > 5) ? argv[5] : NULL;
        JsonObject *trace;

        if (agent == NULL) {
            g_printerr("Usage: clawtilla teach start <agent> [kind] "
                       "[goal]\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "teach.start",
                     build_payload("agent", agent, "source", kind,
                                   "goal", goal, NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        trace = json_node_get_object(reply);
        print_teach_caveats(trace);
        g_print("Recording %s. Stop it with `clawtilla teach stop %s`.\n",
                member_or(trace, "id", "?"), member_or(trace, "id", "?"));

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "stop") == 0) {
        JsonObject *trace;

        if (id == NULL) {
            g_printerr("Usage: clawtilla teach stop <id>\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "teach.stop", build_payload("id", id, NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        trace = json_node_get_object(reply);
        g_print("Stopped. %" G_GINT64_FORMAT " step(s)",
                json_object_get_int_member(trace, "step_count"));

        if (json_object_get_int_member(trace, "dropped") > 0)
            g_print(", %" G_GINT64_FORMAT " not recorded",
                    json_object_get_int_member(trace, "dropped"));

        g_print(". `clawtilla teach show %s` to read it.\n", id);

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "show") == 0) {
        JsonObject *trace;
        JsonArray *steps;

        if (id == NULL) {
            g_printerr("Usage: clawtilla teach show <id>\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "teach.show", build_payload("id", id, NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        trace = json_node_get_object(reply);
        print_teach_caveats(trace);

        g_print("%s  %s  agent %s\n", member_or(trace, "id", "?"),
                member_or(trace, "source", "?"),
                member_or(trace, "agent", "-"));

        if (member_or(trace, "goal", NULL) != NULL)
            g_print("Teaching: %s\n", member_or(trace, "goal", ""));

        if (json_object_get_int_member(trace, "dropped") > 0)
            g_print("%" G_GINT64_FORMAT " step(s) were not recorded: the "
                    "recording hit skills.teach_max_events.\n",
                    json_object_get_int_member(trace, "dropped"));

        if (json_object_get_int_member(trace, "suppressed") > 0)
            g_print("%" G_GINT64_FORMAT " event(s) were withheld while "
                    "capture was paused.\n",
                    json_object_get_int_member(trace, "suppressed"));

        steps = json_object_get_array_member(trace, "steps");
        g_print("\n");

        for (i = 0; steps != NULL && i < json_array_get_length(steps); i++) {
            JsonObject *step = json_array_get_object_element(steps, i);

            g_print("%3u. [%s] %s\n", i + 1, member_or(step, "kind", "?"),
                    member_or(step, "label", ""));

            if (member_or(step, "detail", NULL) != NULL)
                g_print("     %s\n", member_or(step, "detail", ""));
        }

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "rm") == 0 || g_strcmp0(verb, "remove") == 0) {
        if (id == NULL) {
            g_printerr("Usage: clawtilla teach rm <id>\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "teach.remove", build_payload("id", id, NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("Removed %s.\n", id);

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "draft") == 0 ||
        g_strcmp0(verb, "synthesize") == 0) {
        JsonObject *result;

        if (id == NULL) {
            g_printerr("Usage: clawtilla teach draft <id>\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "teach.synthesize",
                     build_payload("id", id, "provider",
                                   (argc > 4) ? argv[4] : NULL, NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        result = json_node_get_object(reply);

        g_print("%s\n", member_or(result, "preview", ""));
        g_print("\n%s\n", member_or(result, "note", ""));
        g_print("`clawtilla teach commit %s` writes it.\n", id);

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "commit") == 0) {
        JsonObject *result;

        if (id == NULL) {
            g_printerr("Usage: clawtilla teach commit <id>\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "teach.commit", build_payload("id", id, NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        result = json_node_get_object(reply);

        g_print("Wrote skill '%s'.\n%s\n", member_or(result, "name", "?"),
                member_or(result, "note", ""));

        return EXIT_SUCCESS;
    }

    g_printerr("clawtilla: unknown teach verb '%s'\n", verb);
    print_teach_usage(FALSE);
    return EXIT_FAILURE;
}

/* @asked_for: see print_usage_text(). */
static void
print_routine_usage(gboolean asked_for)
{
    static const gchar *text =
        "Usage: clawtilla routine <verb> [...]\n"
        "\n"
        "  list                           what is scheduled, and when next\n"
        "  add <id> <agent> [key=value]   add one\n"
        "  set <id> [key=value ...]       change one\n"
        "  rm <id>                        remove it\n"
        "  run <id>                       run it now, schedule or not\n"
        "\n"
        "Schedules: manual, hourly, daily, weekdays, weekly, custom.\n"
        "\n"
        "Examples:\n"
        "  clawtilla routine add standup chief-of-staff \\\n"
        "      description=\"Yesterday's commits\" \\\n"
        "      instructions=\"Summarise the last 24 hours of commits.\" \\\n"
        "      schedule=weekdays at=09:00\n"
        "  clawtilla routine add sweep researcher schedule=custom \\\n"
        "      cron=\"0 */6 * * *\" instructions=\"Check the queue.\"\n"
        "  clawtilla routine run standup\n";

    print_usage_text(text, asked_for);
}

static gint
cmd_routine(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    const gchar *verb = (argc > 2) ? argv[2] : "list";
    const gchar *id = (argc > 3) ? argv[3] : NULL;
    guint i;

    if (g_strcmp0(verb, "help") == 0 || g_strcmp0(verb, "--help") == 0 ||
        g_strcmp0(verb, "-h") == 0) {
        print_routine_usage(TRUE);
        return EXIT_SUCCESS;
    }

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    if (g_strcmp0(verb, "list") == 0) {
        JsonArray *routines;

        reply = call(client, "routine.list", NULL);
        if (reply == NULL)
            return EXIT_FAILURE;

        routines = json_object_get_array_member(json_node_get_object(reply),
                                                "routines");

        if (json_array_get_length(routines) == 0) {
            g_print("Nothing scheduled. `clawtilla routine add` makes "
                    "one.\n");
            return EXIT_SUCCESS;
        }

        g_print("%-18s %-16s %-16s %-20s %s\n", "ID", "AGENT", "SCHEDULE",
                "NEXT", "LAST");

        for (i = 0; i < json_array_get_length(routines); i++) {
            JsonObject *routine = json_array_get_object_element(routines, i);
            const gchar *next = member_or(routine, "next_run", NULL);
            g_autofree gchar *when = NULL;
            g_autofree gchar *schedule = NULL;

            schedule = g_strdup_printf(
                "%s%s", member_or(routine, "schedule", "?"),
                json_object_get_boolean_member(routine, "enabled")
                    ? "" : " (off)");

            /*
             * The expression is shown for a custom schedule, because
             * "custom" on its own answers nothing.
             */
            if (g_str_has_prefix(schedule, "custom")) {
                g_free(schedule);
                schedule = g_strdup(member_or(routine, "expression", "?"));
            }

            /*
             * A routine that cannot be scheduled says so here rather
             * than showing "-" under NEXT, which is also what a manual
             * routine shows.  The daemon has answered this as `problem`
             * since routine.list was written and this client never read
             * it, so a routine that would never fire read as healthy --
             * the reason one sat broken long enough to write 1,368 log
             * lines a day.
             */
            if (next != NULL) {
                g_autoptr(GDateTime) parsed =
                    g_date_time_new_from_iso8601(next, NULL);

                when = (parsed != NULL)
                    ? g_date_time_format(parsed, "%a %d %b %H:%M")
                    : g_strdup(next);
            } else if (member_or(routine, "problem", NULL) != NULL) {
                when = g_strdup("never");
            } else {
                when = g_strdup("-");
            }

            g_print("%-18s %-16s %-16s %-20s %s%s%s\n",
                    member_or(routine, "id", "?"),
                    member_or(routine, "agent", "?"), schedule, when,
                    member_or(routine, "last_state", "never"),
                    member_or(routine, "last_detail", NULL) != NULL
                        ? ": " : "",
                    member_or(routine, "last_detail", ""));

            /*
             * On its own line, at full width: the message names the
             * value that could not be read, and truncating it into a
             * column would take away the only thing that says which
             * character to fix.
             */
            if (member_or(routine, "problem", NULL) != NULL)
                g_printerr("  %s: %s\n", member_or(routine, "id", "?"),
                           member_or(routine, "problem", ""));
        }

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "add") == 0 || g_strcmp0(verb, "set") == 0) {
        gboolean adding = g_strcmp0(verb, "add") == 0;
        g_autoptr(JsonBuilder) builder = json_builder_new();
        int first = adding ? 5 : 4;
        int a;

        if (id == NULL || (adding && argc < 5)) {
            g_printerr("Usage: clawtilla routine %s <id>%s [key=value ...]\n",
                       verb, adding ? " <agent>" : "");
            return EXIT_FAILURE;
        }

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "id");
        json_builder_add_string_value(builder, id);

        if (adding) {
            json_builder_set_member_name(builder, "agent");
            json_builder_add_string_value(builder, argv[4]);
        }

        for (a = first; a < argc; a++) {
            g_auto(GStrv) parts = g_strsplit(argv[a], "=", 2);
            g_autofree gchar *schema_key = NULL;
            const ClawtSchemaEntry *entry;

            if (parts[0] == NULL || parts[1] == NULL) {
                g_printerr("clawtilla: '%s' is not key=value\n", argv[a]);
                return EXIT_FAILURE;
            }

            schema_key = g_strdup_printf("routines.%s", parts[0]);
            entry = clawt_config_schema_lookup(schema_key);

            if (entry == NULL) {
                g_printerr("clawtilla: '%s' is not something a routine "
                           "holds\n", parts[0]);
                return EXIT_FAILURE;
            }

            json_builder_set_member_name(builder, parts[0]);

            switch (entry->type) {
            case CLAWT_SCHEMA_BOOLEAN:
                json_builder_add_boolean_value(
                    builder, g_strcmp0(parts[1], "true") == 0 ||
                             g_strcmp0(parts[1], "yes") == 0 ||
                             g_strcmp0(parts[1], "1") == 0);
                break;

            case CLAWT_SCHEMA_INT:
                json_builder_add_int_value(
                    builder, g_ascii_strtoll(parts[1], NULL, 10));
                break;

            default:
                json_builder_add_string_value(builder, parts[1]);
                break;
            }
        }

        json_builder_end_object(builder);

        reply = call(client, adding ? "routine.add" : "routine.update",
                     json_builder_get_root(builder));

        return reply != NULL ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (g_strcmp0(verb, "rm") == 0) {
        if (id == NULL) {
            g_printerr("Usage: clawtilla routine rm <id>\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "routine.remove", build_payload("id", id, NULL));
        return reply != NULL ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (g_strcmp0(verb, "run") == 0) {
        if (id == NULL) {
            g_printerr("Usage: clawtilla routine run <id>\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "routine.run", build_payload("id", id, NULL));

        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("Started as task %s.\n",
                member_or(json_node_get_object(reply), "task", "?"));
        return EXIT_SUCCESS;
    }

    g_printerr("clawtilla: unknown routine verb '%s'\n", verb);
    print_routine_usage(FALSE);
    return EXIT_FAILURE;
}

static void
print_trigger_usage(gboolean asked_for)
{
    static const gchar *text =
        "Usage: clawtilla trigger <verb> [...]\n"
        "\n"
        "  list                           what can start a run, and where\n"
        "  add <id> <agent> [key=value]   add one; prints the secret ONCE\n"
        "  set <id> [key=value ...]       change one\n"
        "  rm <id>                        remove it\n"
        "  rotate <id>                    new secret and new address\n"
        "  test <id> [--run]              show the prompt, or run it\n"
        "  capture <id>                   the first delivery, as it arrived\n"
        "  deliveries [id]                what has been sent, and what came\n"
        "                                 of it\n"
        "\n"
        "Providers: forgejo, gitea, github, gitlab, generic.\n"
        "\n"
        "A new trigger starts switched off and unverified: the first\n"
        "delivery that authenticates is captured for you to look at rather\n"
        "than run. Read it with `capture`, then `set <id> enabled=true`.\n"
        "\n"
        "The secret is printed once, when it is created or rotated, and\n"
        "never again -- not by `list`, not in a log, not in an event. If\n"
        "you lose it, rotate.\n"
        "\n"
        "Examples:\n"
        "  clawtilla trigger add ci-failed builder \\\n"
        "      provider=forgejo repo=zach/clawtilla events=push \\\n"
        "      instructions=\"A push landed on {{repo}} {{ref}} by\n"
        "                    {{actor}}. Run the tests and report.\"\n"
        "  clawtilla trigger capture ci-failed\n"
        "  clawtilla trigger set ci-failed enabled=true\n"
        "  clawtilla trigger deliveries ci-failed\n";

    print_usage_text(text, asked_for);
}

/*
 * Prints a secret exactly once, with the sentence that makes that
 * survivable.
 *
 * Somebody who does not know it is the only copy will close the terminal
 * and then ask where to find it again -- and the honest answer is
 * "nowhere", so the message has to say so at the moment it is readable.
 */
static void
print_secret_once(JsonObject *reply)
{
    const gchar *secret = member_or(reply, "secret", NULL);
    const gchar *endpoint = member_or(reply, "endpoint", NULL);

    if (endpoint != NULL)
        g_print("Endpoint: %s\n", endpoint);

    if (secret == NULL)
        return;

    g_print("Secret:   %s\n", secret);
    g_print("\n");
    g_print("That is the only time this is printed. Put it in the "
            "webhook's secret field now;\nif you lose it, "
            "`clawtilla trigger rotate` makes a new one and retires "
            "this.\n");
}

static gint
cmd_trigger(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    const gchar *verb = (argc > 2) ? argv[2] : "list";
    const gchar *id = (argc > 3) ? argv[3] : NULL;
    guint i;

    if (g_strcmp0(verb, "help") == 0 || g_strcmp0(verb, "--help") == 0 ||
        g_strcmp0(verb, "-h") == 0) {
        print_trigger_usage(TRUE);
        return EXIT_SUCCESS;
    }

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    if (g_strcmp0(verb, "list") == 0) {
        JsonArray *triggers;
        JsonObject *root;

        reply = call(client, "trigger.list", NULL);
        if (reply == NULL)
            return EXIT_FAILURE;

        root = json_node_get_object(reply);
        triggers = json_object_get_array_member(root, "triggers");

        if (json_array_get_length(triggers) == 0) {
            g_print("Nothing is waiting on an event. "
                    "`clawtilla trigger add` makes one.\n");
            return EXIT_SUCCESS;
        }

        /*
         * Said once, at the top, rather than per row: with the receiver
         * off every trigger is equally unreachable, and repeating it
         * would read as a per-trigger fault.
         */
        if (!member_flag(root, "receiving", FALSE))
            g_print("The receiver is not running, so nothing can arrive. "
                    "Set daemon.webhook_enabled.\n\n");

        g_print("%-18s %-16s %-10s %-8s %s\n", "ID", "AGENT", "PROVIDER",
                "STATE", "ENDPOINT");

        for (i = 0; i < json_array_get_length(triggers); i++) {
            JsonObject *trigger = json_array_get_object_element(triggers, i);
            const gchar *state;

            /*
             * One word, in priority order, rather than three flags a
             * reader has to combine: "why is this not firing" wants one
             * answer, and the first true one is always the answer.
             */
            if (!member_flag(trigger, "has_secret", FALSE))
                state = "NO KEY";
            else if (member_flag(trigger, "pending_verification", FALSE))
                state = "unverified";
            else if (!member_flag(trigger, "enabled", FALSE))
                state = "off";
            else
                state = "on";

            g_print("%-18s %-16s %-10s %-8s %s\n",
                    member_or(trigger, "id", "?"),
                    member_or(trigger, "agent", "?"),
                    member_or(trigger, "provider", "generic"), state,
                    member_or(trigger, "endpoint", "-"));
        }

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "add") == 0 || g_strcmp0(verb, "set") == 0) {
        gboolean adding = g_strcmp0(verb, "add") == 0;
        g_autoptr(JsonBuilder) builder = json_builder_new();
        int first = adding ? 5 : 4;
        int a;

        if (id == NULL || (adding && argc < 5)) {
            g_printerr("Usage: clawtilla trigger %s <id>%s [key=value ...]\n",
                       verb, adding ? " <agent>" : "");
            return EXIT_FAILURE;
        }

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "id");
        json_builder_add_string_value(builder, id);

        if (adding) {
            json_builder_set_member_name(builder, "agent");
            json_builder_add_string_value(builder, argv[4]);
        }

        for (a = first; a < argc; a++) {
            g_auto(GStrv) parts = g_strsplit(argv[a], "=", 2);
            g_autofree gchar *schema_key = NULL;
            const ClawtSchemaEntry *entry;

            if (parts[0] == NULL || parts[1] == NULL) {
                g_printerr("clawtilla: '%s' is not key=value\n", argv[a]);
                return EXIT_FAILURE;
            }

            schema_key = g_strdup_printf("triggers.%s", parts[0]);
            entry = clawt_config_schema_lookup(schema_key);

            if (entry == NULL) {
                g_printerr("clawtilla: '%s' is not something a trigger "
                           "holds\n", parts[0]);
                return EXIT_FAILURE;
            }

            /*
             * There is deliberately no way to supply a secret. An
             * argument is in the shell history and in the process
             * table, and the daemon mints its own from /dev/urandom.
             */
            if (entry->type == CLAWT_SCHEMA_SECRET) {
                g_printerr("clawtilla: a trigger's secret is generated, "
                           "not given -- it would be in your shell "
                           "history and in `ps`\n");
                return EXIT_FAILURE;
            }

            json_builder_set_member_name(builder, parts[0]);

            switch (entry->type) {
            case CLAWT_SCHEMA_BOOLEAN:
                json_builder_add_boolean_value(
                    builder, g_strcmp0(parts[1], "true") == 0 ||
                             g_strcmp0(parts[1], "yes") == 0 ||
                             g_strcmp0(parts[1], "1") == 0);
                break;

            case CLAWT_SCHEMA_INT:
                json_builder_add_int_value(
                    builder, g_ascii_strtoll(parts[1], NULL, 10));
                break;

            default:
                /*
                 * A list goes over as a string; the daemon splits it,
                 * because it is the side that knows the key is a list.
                 */
                json_builder_add_string_value(builder, parts[1]);
                break;
            }
        }

        json_builder_end_object(builder);

        reply = call(client, adding ? "trigger.add" : "trigger.update",
                     json_builder_get_root(builder));

        if (reply == NULL)
            return EXIT_FAILURE;

        print_secret_once(json_node_get_object(reply));

        if (adding)
            g_print("\nIt is switched off until you have seen a delivery. "
                    "Point the webhook at\nthe endpoint above, then "
                    "`clawtilla trigger capture %s`.\n", id);

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "rm") == 0) {
        if (id == NULL) {
            g_printerr("Usage: clawtilla trigger rm <id>\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "trigger.remove", build_payload("id", id, NULL));
        return reply != NULL ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (g_strcmp0(verb, "rotate") == 0) {
        if (id == NULL) {
            g_printerr("Usage: clawtilla trigger rotate <id>\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "trigger.rotate", build_payload("id", id, NULL));

        if (reply == NULL)
            return EXIT_FAILURE;

        print_secret_once(json_node_get_object(reply));
        g_print("\nThe old secret and the old address stopped working just "
                "now, so the webhook\nneeds both of the above before it "
                "will be accepted again.\n");

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "capture") == 0) {
        JsonObject *root;
        const gchar *payload;

        if (id == NULL) {
            g_printerr("Usage: clawtilla trigger capture <id>\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "trigger.capture", build_payload("id", id, NULL));

        if (reply == NULL)
            return EXIT_FAILURE;

        root = json_node_get_object(reply);
        payload = member_or(root, "payload", NULL);

        if (payload == NULL) {
            g_print("Nothing has arrived yet. Point the webhook at this "
                    "trigger's endpoint\nand send it a test delivery; "
                    "the first one that authenticates is held here.\n");
            return EXIT_SUCCESS;
        }

        g_print("%s\n", payload);

        if (!member_flag(root, "pending_verification", TRUE))
            g_print("\nThat is what the caller sent. If it is what you "
                    "expected, switch the trigger\non with `clawtilla "
                    "trigger set %s enabled=true`.\n", id);

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "test") == 0) {
        g_autoptr(JsonBuilder) builder = json_builder_new();
        gboolean run = FALSE;
        JsonObject *root;
        int a;

        if (id == NULL) {
            g_printerr("Usage: clawtilla trigger test <id> [--run]\n");
            return EXIT_FAILURE;
        }

        for (a = 4; a < argc; a++) {
            if (g_strcmp0(argv[a], "--run") == 0)
                run = TRUE;
        }

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "id");
        json_builder_add_string_value(builder, id);
        json_builder_set_member_name(builder, "run");
        json_builder_add_boolean_value(builder, run);
        json_builder_end_object(builder);

        reply = call(client, "trigger.test", json_builder_get_root(builder));

        if (reply == NULL)
            return EXIT_FAILURE;

        root = json_node_get_object(reply);

        if (run) {
            g_print("Started as task %s.\n", member_or(root, "task", "?"));
            return EXIT_SUCCESS;
        }

        g_print("%s\n", member_or(root, "prompt", ""));
        g_print("\nThat is what the agent would be asked. "
                "`--run` sends it.\n");

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "deliveries") == 0) {
        JsonArray *deliveries;
        JsonObject *root;

        reply = call(client, "trigger.deliveries",
                     id != NULL ? build_payload("id", id, NULL) : NULL);

        if (reply == NULL)
            return EXIT_FAILURE;

        root = json_node_get_object(reply);
        deliveries = json_object_get_array_member(root, "deliveries");

        if (json_array_get_length(deliveries) == 0) {
            g_print("%s\n", member_or(root, "note",
                                      "nothing has been delivered yet"));
            return EXIT_SUCCESS;
        }

        g_print("%-20s %-16s %-14s %-10s %s\n", "WHEN", "TRIGGER", "EVENT",
                "OUTCOME", "DETAIL");

        for (i = 0; i < json_array_get_length(deliveries); i++) {
            JsonObject *row = json_array_get_object_element(deliveries, i);
            gint64 at = g_ascii_strtoll(member_or(row, "at", "0"), NULL, 10);
            g_autoptr(GDateTime) when = g_date_time_new_from_unix_local(at);
            g_autofree gchar *stamp =
                (when != NULL) ? g_date_time_format(when, "%a %d %b %H:%M")
                               : g_strdup("-");

            g_print("%-20s %-16s %-14s %-10s %s\n", stamp,
                    member_or(row, "trigger", "?"),
                    member_or(row, "event", "-"),
                    member_or(row, "outcome", "?"),
                    member_or(row, "detail", ""));
        }

        return EXIT_SUCCESS;
    }

    g_printerr("clawtilla: unknown trigger verb '%s'\n", verb);
    print_trigger_usage(FALSE);
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
cmd_status(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    JsonObject *status;

    /* Takes no arguments; it is in the verb table, so it has the shape. */
    (void)argc;
    (void)argv;

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    reply = call(client, "control.status", NULL);
    if (reply == NULL)
        return EXIT_FAILURE;

    status = json_node_get_object(reply);

    g_print("clawtillad %s (%s)\n", member_or(status, "version", "?"),
            member_or(status, "commit", "unknown build"));
    g_print("config:    %s\n", member_or(status, "config", "?"));

    /*
     * Whether a newer one exists, and -- just as important -- whether
     * the check itself is working.  A check that has been erroring for a
     * month is worse than no check, because a client with nothing to
     * draw draws nothing and that reads as "up to date".
     *
     * Nothing at all is printed when the member is absent, which is the
     * default: not checking and checking-and-finding-nothing are
     * different states and only one of them deserves a line.
     */
    /*
     * Whether the fleet is held, and whether it has finished draining.
     * Printed above the counts because it changes what they mean: 24
     * agents under a hold are 24 agents that are not taking anything.
     */
    if (json_object_has_member(status, "hold")) {
        JsonObject *hold = json_object_get_object_member(status, "hold");

        if (json_object_get_boolean_member(hold, "held")) {
            gint64 draining = json_object_get_int_member(hold, "draining");

            if (draining > 0)
                g_print("hold:      draining -- %" G_GINT64_FORMAT
                        " turn(s) still in flight\n", draining);
            else
                g_print("hold:      held, nothing in flight -- safe to "
                        "restart\n");
        }
    }

    if (json_object_has_member(status, "update")) {
        JsonObject *update = json_object_get_object_member(status, "update");

        if (json_object_get_boolean_member(update, "available"))
            g_print("update:    %s is available\n",
                    member_or(update, "latest", "?"));
        else if (member_or(update, "error", NULL) != NULL)
            g_print("update:    the check is failing: %s\n",
                    member_or(update, "error", ""));
        else if (json_object_get_int_member(update, "checked_at") == 0)
            g_print("update:    on, but it has not run yet\n");
        else
            g_print("update:    up to date\n");
    }
    g_print("agents:    %" G_GINT64_FORMAT " (%" G_GINT64_FORMAT
            " connected)\n",
            json_object_get_int_member(status, "agents"),
            json_object_get_int_member(status, "connected"));
    g_print("clients:   %" G_GINT64_FORMAT "\n",
            json_object_get_int_member(status, "clients"));

    /*
     * Which agents those are, and what each is doing.
     *
     * "24 (2 connected)" cannot be acted on: it says the fleet is partly
     * up without saying which part, so the next question is always
     * another command.  A second call rather than a wider control.status
     * -- the count is what that verb is for, and every client already
     * calls agent.list.
     *
     * A failure here is not a failure of `status`: the counts above are
     * this command's answer and they have already been printed.  So the
     * reply is used when it arrives and passed over when it does not,
     * rather than turning a working status into an error.
     */
    {
        g_autoptr(JsonNode) fleet = call(client, "agent.list", NULL);

        if (fleet != NULL) {
            JsonArray *agents = json_object_get_array_member(
                json_node_get_object(fleet), "agents");
            guint i;
            gboolean any = FALSE;

            for (i = 0; i < json_array_get_length(agents); i++) {
                JsonObject *agent = json_array_get_object_element(agents, i);
                g_autofree gchar *work = NULL;

                if (!json_object_get_boolean_member(agent, "connected"))
                    continue;

                work = clawt_agent_activity_label(
                    json_object_has_member(agent, "busy") &&
                        json_object_get_boolean_member(agent, "busy"),
                    member_or(agent, "peer", NULL));

                if (!any) {
                    g_print("\n");
                    any = TRUE;
                }

                g_print("  %-20s %s\n", member_or(agent, "id", "?"),
                        work != NULL ? work : "idle");
            }
        }
    }

    return EXIT_SUCCESS;
}

/*
 * `clawtilla pause [agent]` and `clawtilla resume [agent]`.
 *
 * One function, because the two are the same shape and the only thing
 * that differs is the verb and the sentence -- and a second copy of the
 * "which agent, and did it work" handling is a second place for them to
 * disagree about the fleet case.
 */
static gint
cmd_hold(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    gboolean pausing = g_strcmp0(argv[1], "pause") == 0;
    const gchar *agent = (argc > 2) ? argv[2] : NULL;
    JsonObject *hold;
    gint64 draining;

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    reply = call(client, pausing ? "control.pause" : "control.resume",
                 agent != NULL ? build_payload("agent", agent, NULL) : NULL);

    if (reply == NULL)
        return EXIT_FAILURE;

    hold = json_object_get_object_member(json_node_get_object(reply), "hold");

    if (!pausing) {
        g_print("Resumed. Anything queued goes out now, in order.\n");
        return EXIT_SUCCESS;
    }

    draining = json_object_get_int_member(hold, "draining");

    /*
     * The count is the answer, and the point of the whole feature: it
     * turns "restarting loses whatever everything happened to be doing"
     * into a number that reaches zero, which is a defined moment at
     * which a restart is safe.  Nothing here waits for it -- an IPC
     * handler must not, and neither should a command somebody may want
     * to walk away from.
     */
    if (draining > 0)
        g_print("Held. Draining: %" G_GINT64_FORMAT " turn(s) still in "
                "flight.\nRun `clawtilla status` until it reaches zero; "
                "queued work stays queued.\n", draining);
    else
        g_print("Held, with nothing in flight. It is safe to restart; "
                "queued work stays queued and\nwhat was running comes "
                "back by itself.\n");

    return EXIT_SUCCESS;
}

static gint cmd_daemon_token(void);

static gint
cmd_daemon(int argc, char *argv[])
{
    g_autoptr(ClawtDaemon) daemon = NULL;

    if (argc > 2 && g_strcmp0(argv[2], "token") == 0)
        return cmd_daemon_token();

    /*
     * The same daemon clawtillad runs, in the foreground.  Handy for a
     * quick session without installing a service, and it is the identical
     * code path rather than a second implementation that can drift.
     */
    daemon = clawt_daemon_new(opt_config_path, NULL);

    return clawt_daemon_run(daemon);
}

/*
 * Prints the token a remote client must present to this machine's daemon.
 *
 * Read from the file rather than asked for over IPC, and deliberately.
 * Nothing may write a secret's value into an IPC response -- so there is
 * no `daemon.token` request to make, and the value is taken from disk by
 * a command that only runs where the daemon does.
 */
static gint
cmd_daemon_token(void)
{
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *configured = NULL;
    g_autofree gchar *path = NULL;
    g_autofree gchar *token = NULL;

    config = clawt_config_load(opt_config_path, &error);

    if (config == NULL) {
        g_printerr("clawtilla: %s\n", error->message);
        return EXIT_FAILURE;
    }

    configured = clawt_config_get_path_value(config, "daemon.token_file");

    if (configured != NULL && *configured != '\0') {
        path = g_steal_pointer(&configured);
    } else {
        g_autofree gchar *state_dir =
            clawt_config_get_path_value(config, "daemon.state_dir");

        path = g_build_filename(state_dir, "tcp-token", NULL);
    }

    if (!g_file_get_contents(path, &token, NULL, &error)) {
        g_printerr("clawtilla: no token yet (%s)\n", path);
        g_printerr("  One is generated the first time the daemon starts a\n"
                   "  TCP or tailnet listener. Check daemon.tailscale is on\n"
                   "  and that this machine has a tailnet address.\n");
        return EXIT_FAILURE;
    }

    g_strstrip(token);
    g_print("%s\n", token);

    return EXIT_SUCCESS;
}

/*
 * Saved connections, shared with the GTK client's connection menu.
 *
 * `list` never prints a token.  It is the command a person runs to check
 * what they saved, often over somebody's shoulder, and `daemon token` on
 * the machine that owns it is the way to see one.
 */
static gint
cmd_remote(int argc, char *argv[])
{
    const gchar *verb = (argc > 2) ? argv[2] : "list";
    g_autoptr(GPtrArray) connections = NULL;
    g_autoptr(GError) error = NULL;
    guint i;

    connections = clawt_connection_list_load(NULL, &error);

    if (connections == NULL) {
        g_printerr("clawtilla: %s\n", error->message);
        return EXIT_FAILURE;
    }

    if (g_strcmp0(verb, "list") == 0) {
        g_autofree gchar *path = clawt_connection_list_default_path();

        if (connections->len == 0) {
            g_print("No saved connections.\n");
            g_print("  clawtilla remote add <name> <host> [--port N] "
                    "[--token T]\n");
            return EXIT_SUCCESS;
        }

        for (i = 0; i < connections->len; i++) {
            ClawtConnection *connection = g_ptr_array_index(connections, i);
            g_autofree gchar *where = clawt_connection_describe(connection);

            g_print("%-20s %s%s\n", clawt_connection_get_name(connection),
                    where,
                    clawt_connection_get_token(connection) != NULL
                        ? "" : "   (no token)");
        }

        g_print("\nFrom %s\n", path);

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "add") == 0) {
        const gchar *name = (argc > 3) ? argv[3] : NULL;
        const gchar *host = (argc > 4) ? argv[4] : NULL;
        gint64 port = CLAWT_DEFAULT_TCP_PORT;
        const gchar *token = NULL;
        gboolean tls = FALSE;
        gboolean insecure = FALSE;
        ClawtConnection *connection;
        gint arg;

        if (name == NULL || host == NULL) {
            g_printerr("Usage: clawtilla remote add <name> <host> "
                       "[--port N] [--token T] [--tls] [--insecure]\n");
            g_printerr("  e.g. clawtilla remote add workstation "
                       "100.72.0.41 --token \"$(ssh box clawtilla daemon "
                       "token)\"\n");
            return EXIT_FAILURE;
        }

        for (arg = 5; arg < argc; arg++) {
            if (g_strcmp0(argv[arg], "--port") == 0 && arg + 1 < argc)
                port = g_ascii_strtoll(argv[++arg], NULL, 10);
            else if (g_strcmp0(argv[arg], "--token") == 0 && arg + 1 < argc)
                token = argv[++arg];
            else if (g_strcmp0(argv[arg], "--tls") == 0)
                tls = TRUE;
            else if (g_strcmp0(argv[arg], "--insecure") == 0)
                insecure = TRUE;
        }

        if (port <= 0 || port > G_MAXUINT16) {
            g_printerr("clawtilla: %" G_GINT64_FORMAT " is not a port\n",
                       port);
            return EXIT_FAILURE;
        }

        if (clawt_connection_list_find(connections, name) != NULL) {
            g_printerr("clawtilla: there is already a connection called "
                       "'%s'\n", name);
            return EXIT_FAILURE;
        }

        connection = clawt_connection_new_remote(name, host, (guint16)port,
                                                  token);
        clawt_connection_set_tls(connection, tls, insecure);
        g_ptr_array_add(connections, connection);

        if (!clawt_connection_list_save(NULL, connections, &error)) {
            g_printerr("clawtilla: %s\n", error->message);
            return EXIT_FAILURE;
        }

        g_print("Saved %s.\n", name);

        if (token == NULL)
            g_print("No token given; that daemon will refuse the "
                    "connection until one is.\n");

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "rm") == 0) {
        const gchar *name = (argc > 3) ? argv[3] : NULL;
        ClawtConnection *found;

        if (name == NULL) {
            g_printerr("Usage: clawtilla remote rm <name>\n");
            return EXIT_FAILURE;
        }

        found = clawt_connection_list_find(connections, name);

        if (found == NULL) {
            g_printerr("clawtilla: there is no connection called '%s'\n",
                       name);
            return EXIT_FAILURE;
        }

        g_ptr_array_remove(connections, found);

        if (!clawt_connection_list_save(NULL, connections, &error)) {
            g_printerr("clawtilla: %s\n", error->message);
            return EXIT_FAILURE;
        }

        g_print("Removed %s.\n", name);

        return EXIT_SUCCESS;
    }

    g_printerr("Usage: clawtilla remote {list,add,rm}\n");

    return EXIT_FAILURE;
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

/* ── the verb table ──────────────────────────────────────────────── */

/*
 * Every verb, once.
 *
 * Three things need to know what the verbs are: main() dispatches on
 * them, find_verb() uses them to work out where the global options stop,
 * and --help lists them.  Those were three hand-written lists, and the
 * help was the one nobody edited -- nine verbs were dispatched and
 * documented and could not be discovered from the CLI itself, `event`
 * among them, which is the whole difference between diagnosing a message
 * loop with a command and doing it with sqlite3 on the host.
 *
 * A verb added here is dispatched, parsed around and listed by
 * construction.  There is nowhere left to add one and forget.
 */
typedef struct {
    const gchar *name;
    const gchar *args;      /* what follows the verb in the listing */
    const gchar *summary;
    gint       (*run)(int argc, char *argv[]);
} ClawtVerb;

/*
 * The order is the order --help prints, since nothing else cares: the
 * dispatcher matches on the name and find_verb() scans the whole table.
 */
static const ClawtVerb verbs[] = {
    { "daemon",  "[token]",  "Run the daemon, or print its token",
      cmd_daemon },
    { "remote",  "<verb>",   "Saved connections to other machines",
      cmd_remote },
    { "status",  "",         "What this daemon is running right now",
      cmd_status },
    { "pause",   "[agent]",  "Hold the fleet: stop delivery, let turns finish",
      cmd_hold },
    { "resume",  "[agent]",  "Take the hold off and deliver what queued up",
      cmd_hold },
    { "agent",   "<verb>",   "Manage agents (list, show, create, ...)",
      cmd_agent },
    { "send",    "<target> <message>", "Send a message to an agent or room",
      cmd_send },
    { "chat",    "<agent>",  "Interactive chat with one agent",
      cmd_chat },
    { "mailbox", "<verb>",   "Inspect and manage an agent's mailbox",
      cmd_mailbox },
    { "room",    "<verb>",   "Manage rooms",
      cmd_room },
    { "team",    "<verb>",   "Manage teams and who is on them",
      cmd_team },
    { "task",    "<verb>",   "Inspect delegated tasks",
      cmd_task },
    { "decision", "<verb>",  "Answer what an agent has asked you",
      cmd_decision },
    { "event",   "<verb>",   "The daemon's event history",
      cmd_event },
    { "cost",    "[--days N]", "What the fleet has spent, per agent",
      cmd_cost },
    { "memory",  "<verb>",   "What an agent has written down",
      cmd_memory },
    { "computer", "<verb>",  "Run commands on an agent's computer",
      cmd_computer },
    { "cp",      "<src> <dst>", "Copy files to or from a computer",
      cmd_cp },
    { "folders", "<verb>",   "Share host folders with the whole fleet",
      cmd_folders },
    { "config",  "<verb>",   "Show, validate or render configuration",
      cmd_config },
    { "model",   "<verb>",   "Providers and models, and which run agents",
      cmd_model },
    { "image",   "[vm]",     "Container and VM disk images",
      cmd_image },
    { "integration", "<verb>", "Connect agents to Matrix, mail, MCP",
      cmd_integration },
    { "connector", "<verb>", "Accounts agents get tools from",
      cmd_connector },
    { "routine", "<verb>",   "Standing work on a schedule",
      cmd_routine },
    { "trigger", "<verb>",   "Work started by a webhook",
      cmd_trigger },
    { "skill",   "<verb>",   "Procedures agents can be given",
      cmd_skill },
    { "teach",   "<verb>",   "Record a task, and write a skill from it",
      cmd_teach },
    { "plugin",  "list",     "List loaded plugins",
      cmd_plugin },
    { NULL, NULL, NULL, NULL }
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

        for (j = 0; verbs[j].name != NULL; j++) {
            if (g_strcmp0(argv[i], verbs[j].name) == 0)
                return i;
        }
    }

    return 0;
}

/*
 * The whole usage block, with the command listing rendered from the
 * table rather than typed out beside it.
 *
 * Returns: (transfer full): the text to print
 */
static gchar *
build_usage_text(void)
{
    GString *text = g_string_new(usage_head);
    gsize i;

    for (i = 0; verbs[i].name != NULL; i++) {
        g_autofree gchar *left = NULL;

        left = g_strdup_printf("%s%s%s", verbs[i].name,
                               verbs[i].args[0] != '\0' ? " " : "",
                               verbs[i].args);

        /*
         * 30 columns for the name and its arguments, which is what the
         * hand-written block used and what every summary here fits in
         * without the line passing 79 characters.
         */
        g_string_append_printf(text, "  %-30s%s\n", left, verbs[i].summary);
    }

    g_string_append(text, usage_tail);

    return g_string_free(text, FALSE);
}

int
main(int argc, char *argv[])
{
    g_autoptr(GOptionContext) context = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *usage = build_usage_text();

    context = g_option_context_new("- orchestrate a fleet of libreclaw agents");
    g_option_context_add_main_entries(context, entries, NULL);
    g_option_context_set_description(context, usage);

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
        g_printerr("%s", usage);
        return EXIT_FAILURE;
    }

    {
        gsize i;

        for (i = 0; verbs[i].name != NULL; i++) {
            if (g_strcmp0(argv[1], verbs[i].name) == 0)
                return verbs[i].run(argc, argv);
        }
    }

    g_printerr("clawtilla: unknown command '%s'\n", argv[1]);
    g_printerr("Run 'clawtilla --help' for usage.\n");
    return EXIT_FAILURE;
}
