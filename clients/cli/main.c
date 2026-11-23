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
#include "backup.h"

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
#include <errno.h>
#include <ai-glib.h>
#include <stdio.h>
#include <locale.h>
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
    "  clawtilla task list --history --state failed   # kept across restarts\n"
    "  clawtilla task show task-01m0k5 --journal       # what happened to it\n"
    "  clawtilla task create \"Write the release notes\"    # on the backlog\n"
    "  clawtilla task create --agent scout --title \"Fix export\" \"...\"\n"
    "  clawtilla task assign task-01m0k5 scout\n"
    "  clawtilla task comment task-01m0k5 \"use the staging DB\"\n"
    "  clawtilla task create --agent scout --after task-01m0k5 \"Ship it\"\n"
    "  clawtilla task unlink task-01m0k6 task-01m0k5      # stop waiting\n"
    "  clawtilla task review task-01m0k6 changes \"add a test\"\n"
    "  clawtilla task create --agent builder --repo . \"fix the build\"\n"
    "  clawtilla task diff task-01m0k6                    # what it changed\n"
    "  clawtilla task venture-retry task-01m0k6           # send it to VENTURE again\n"
    "  clawtilla task incidents            # broken runs, and who was told\n"
    "  clawtilla task retry task-01m0k5    # send a failed run round again\n"
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
    { "reap", "<agent> [--force]", "end an ephemeral agent; --force drops its open work" },
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
    { "face", "<agent> [SHAPE] [--color HEX]", "its generated face; --list, --svg" },
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

/* ── agent face (0.53.0) ──────────────────────────────────────────── */

static void
print_face_usage(void)
{
    g_printerr("Usage: clawtilla agent face <agent>\n"
               "       clawtilla agent face <agent> <SHAPE|initials|auto|random> "
               "[--color HEX|auto]\n"
               "       clawtilla agent face <agent> --color HEX|auto\n"
               "       clawtilla agent face --list\n"
               "       clawtilla agent face --svg <agent|SHAPE> [--color HEX] "
               "[--size N]\n"
               "\n"
               "Examples:\n"
               "  clawtilla agent face scout cloud --color '#22b573'\n"
               "  clawtilla agent face scout random\n"
               "  clawtilla agent face --svg star --color '#e5484d' > star.svg\n");
}

/* The agent.list entry for @agent_id, borrowed from @reply; NULL if none. */
static JsonObject *
listed_agent(JsonNode *reply, const gchar *agent_id)
{
    JsonArray *agents;
    guint i;

    if (reply == NULL || !JSON_NODE_HOLDS_OBJECT(reply))
        return NULL;

    agents = json_object_get_array_member(json_node_get_object(reply),
                                          "agents");
    for (i = 0; agents != NULL && i < json_array_get_length(agents); i++) {
        JsonObject *agent = json_array_get_object_element(agents, i);

        if (g_strcmp0(member_or(agent, "id", NULL), agent_id) == 0)
            return agent;
    }

    return NULL;
}

/* The value after @flag in argv from @from on, or NULL. */
static const gchar *
face_flag(gint argc, gchar *argv[], gint from, const gchar *flag)
{
    gint i;

    for (i = from; i + 1 < argc; i++) {
        if (g_strcmp0(argv[i], flag) == 0)
            return argv[i + 1];
    }

    return NULL;
}

/*
 * The parts of `agent face` that need no daemon: the shape list, and an
 * SVG of a named shape.  Answered before connecting, so drawing the
 * docs' sheet or checking a spelling works on a machine with no fleet.
 * Returns -1 when the command does need the daemon.
 */
static gint
cmd_agent_face_offline(gint argc, gchar *argv[])
{
    const gchar *first = (argc > 3) ? argv[3] : NULL;
    guint i;

    if (g_strcmp0(first, "--list") == 0) {
        for (i = 0; i < clawt_face_shape_count(); i++)
            g_print("%-10s %s\n", clawt_face_shape_name((gint)i),
                    clawt_face_shape_label((gint)i));
        g_print("%-10s %s\n", CLAWT_FACE_INITIALS_NAME,
                "the letter disc instead of a face");
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(first, "--svg") == 0 && argc > 4 &&
        clawt_face_shape_lookup(argv[4]) >= 0) {
        gint shape = clawt_face_shape_lookup(argv[4]);
        const gchar *color = face_flag(argc, argv, 5, "--color");
        const gchar *size = face_flag(argc, argv, 5, "--size");
        g_autofree gchar *svg = NULL;

        svg = clawt_face_svg(shape,
                             color != NULL ? color
                                           : clawt_face_palette_color(7),
                             size != NULL ? (guint)CLAMP(atoi(size), 8, 4096)
                                          : 128,
                             NULL, clawt_face_seed(argv[4]), FALSE);
        if (svg == NULL) {
            g_printerr("clawtilla: '%s' is not a colour: use #rgb or "
                       "#rrggbb\n", color);
            return EXIT_FAILURE;
        }
        g_print("%s\n", svg);
        return EXIT_SUCCESS;
    }

    if (first == NULL || g_strcmp0(first, "--help") == 0) {
        print_face_usage();
        return (first == NULL) ? EXIT_FAILURE : EXIT_SUCCESS;
    }

    return -1;
}

/* One agent.set; prints the daemon's refusal and answers FALSE. */
static gboolean
face_set(ClawtClient *client, const gchar *agent_id, const gchar *key,
         const gchar *value)
{
    g_autoptr(JsonNode) reply = call(client, "agent.set",
                                     build_payload("agent", agent_id,
                                                   "key", key,
                                                   "value", value, NULL));

    return reply != NULL;
}

static gint
cmd_agent_face(ClawtClient *client, gint argc, gchar *argv[])
{
    const gchar *first = argv[3];
    g_autoptr(JsonNode) listed = NULL;
    JsonObject *agent;

    /* --svg of an agent: its own face and colour, as every client draws */
    if (g_strcmp0(first, "--svg") == 0) {
        const gchar *who = (argc > 4) ? argv[4] : NULL;
        const gchar *color;
        const gchar *size = face_flag(argc, argv, 5, "--size");
        g_autofree gchar *svg = NULL;
        gint shape;

        if (who == NULL) {
            print_face_usage();
            return EXIT_FAILURE;
        }
        listed = call(client, "agent.list", NULL);
        agent = listed_agent(listed, who);
        if (agent == NULL) {
            g_printerr("clawtilla: '%s' is neither a shape nor an agent\n",
                       who);
            return EXIT_FAILURE;
        }
        shape = clawt_face_shape_lookup(member_or(agent, "face", NULL));
        if (shape < 0) {
            g_printerr("clawtilla: %s wears the letter disc, not a face\n",
                       who);
            return EXIT_FAILURE;
        }
        color = face_flag(argc, argv, 5, "--color");
        svg = clawt_face_svg(shape,
                             color != NULL
                                 ? color
                                 : member_or(agent, "color_resolved",
                                             clawt_face_palette_color(7)),
                             size != NULL ? (guint)CLAMP(atoi(size), 8, 4096)
                                          : 128,
                             NULL, clawt_face_seed(who), FALSE);
        if (svg == NULL) {
            g_printerr("clawtilla: '%s' is not a colour\n", color);
            return EXIT_FAILURE;
        }
        g_print("%s\n", svg);
        return EXIT_SUCCESS;
    }

    /* Show: no shape and no --color given */
    if (argc == 4) {
        listed = call(client, "agent.list", NULL);
        agent = listed_agent(listed, first);
        if (agent == NULL) {
            g_printerr("clawtilla: no agent called '%s'\n", first);
            return EXIT_FAILURE;
        }
        g_print("face:   %s%s\n", member_or(agent, "face", "?"),
                member_or(agent, "face_configured", NULL) == NULL
                    ? " (automatic)" : "");
        g_print("colour: %s%s\n", member_or(agent, "color_resolved", "?"),
                member_or(agent, "color", NULL) == NULL ? " (automatic)" : "");
        g_print("picture: %s\n",
                json_object_get_boolean_member_with_default(
                    agent, "has_avatar", FALSE)
                    ? "yes, shown in place of the face" : "none");
        return EXIT_SUCCESS;
    }

    /* Set: a shape (auto = "", random = any shape and colour), --color */
    {
        const gchar *shape_word = (argv[4][0] != '-') ? argv[4] : NULL;
        const gchar *color = face_flag(argc, argv, 4, "--color");
        const gchar *face_value = NULL;
        const gchar *color_value = NULL;

        if (g_strcmp0(shape_word, "random") == 0) {
            gint shape;
            const gchar *picked;

            clawt_face_pick_random(&shape, &picked);
            face_value = clawt_face_shape_name(shape);
            color_value = picked;
        } else if (g_strcmp0(shape_word, "auto") == 0) {
            face_value = "";
        } else if (shape_word != NULL) {
            face_value = shape_word;   /* the daemon checks it */
        }

        if (color != NULL)
            color_value = (g_strcmp0(color, "auto") == 0) ? "" : color;

        if (face_value == NULL && color_value == NULL) {
            print_face_usage();
            return EXIT_FAILURE;
        }
        if (face_value != NULL && !face_set(client, first, "face", face_value))
            return EXIT_FAILURE;
        if (color_value != NULL &&
            !face_set(client, first, "color", color_value))
            return EXIT_FAILURE;

        g_print("%s: face %s, colour %s\n", first,
                face_value == NULL ? "unchanged"
                    : (*face_value == '\0' ? "automatic" : face_value),
                color_value == NULL ? "unchanged"
                    : (*color_value == '\0' ? "automatic" : color_value));
        return EXIT_SUCCESS;
    }
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

    /* the face's list and a shape's SVG need no daemon */
    if (g_strcmp0(verb, "face") == 0) {
        gint offline = cmd_agent_face_offline(argc, argv);

        if (offline >= 0)
            return offline;
    }

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    if (g_strcmp0(verb, "face") == 0)
        return cmd_agent_face(client, argc, argv);

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
            gboolean on_fallback = json_object_has_member(agent, "failover");

            /* a helper says whose it is, and when it is waiting to be reaped */
            g_autofree gchar *helper = NULL;

            if (json_object_has_member(agent, "ephemeral") &&
                json_object_get_boolean_member(agent, "ephemeral"))
                helper = g_strdup_printf("[ephemeral, %s's%s] ",
                    member_or(agent, "parent", "nobody"),
                    json_object_get_boolean_member_with_default(agent,
                        "awaiting_reap", FALSE) ? ", done" : "");

            g_print("%-20s %-10s %-18s %-6" G_GINT64_FORMAT " %-8s %s%s%s\n",
                    member_or(agent, "id", "?"),
                    member_or(agent, "state", "?"),
                    work != NULL ? work : "-",
                    json_object_get_int_member(agent, "mailbox_depth"),
                    json_object_get_boolean_member(agent, "connected")
                        ? "up" : "-",
                    helper != NULL ? helper : "",
                    on_fallback ? "[on fallback] " : "",
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

        /*
         * Where its conversations go when its own backend refuses, and --
         * loudly, because it changes what the agent is answering with --
         * whether it is on one of those right now.
         */
        {
            JsonObject *root = json_node_get_object(reply);
            JsonObject *settings = json_object_has_member(root, "settings")
                ? json_object_get_object_member(root, "settings") : NULL;
            const gchar *fallbacks = settings != NULL
                ? member_or(settings, "model.fallbacks", "") : "";

            if (*fallbacks != '\0')
                g_print("fallbacks:   %s\n", fallbacks);
            if (json_object_has_member(agent, "failover_message"))
                g_print("failover:    %s\n",
                        member_or(agent, "failover_message", ""));
        }

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

        /*
         * What the agent is told is written to the agent; with no
         * computer the operator gets a sentence of their own, and with
         * one the agent's words are introduced as the agent's.
         */
        {
            const gchar *caption = NULL;
            const gchar *said = clawt_agent_computer_text_for_operator(
                member_or(agent, "computer", "none"),
                json_object_has_member(json_node_get_object(reply),
                                       "computer_detail")
                    ? json_object_get_string_member(
                          json_node_get_object(reply), "computer_detail")
                    : NULL,
                &caption);

            if (said != NULL && caption != NULL)
                g_print("\n%s\n%s\n", caption, said);
            else if (said != NULL)
                g_print("\n%s\n", said);
        }

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
                           "[--image X] [--vm-image PATH] [--face SHAPE] "
                           "[--color HEX] [--no-start]\n");
                g_printerr("\n  --face      a shape (clawtilla agent face "
                           "--list), initials, or \"\" for automatic; "
                           "left out, a random one\n");
                g_printerr("  --color     the face's colour, #rgb or "
                           "#rrggbb; left out, a random palette colour\n");
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

    /*
     * The operator may reap any helper; open work is kept unless --force,
     * the same refusal the parent gets.
     */
    if (g_strcmp0(verb, "reap") == 0) {
        gboolean force = argc > 4 && g_strcmp0(argv[4], "--force") == 0;

        if (target == NULL) {
            g_printerr("Usage: clawtilla agent reap <agent> [--force]\n");
            return EXIT_FAILURE;
        }
        reply = call(client, "agent.reap",
                     build_payload("agent", target, "force",
                                   force ? "true" : "false", NULL));
        if (reply == NULL)
            return EXIT_FAILURE;
        g_print("Reaped %s.\n", target);
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
        g_printerr("Usage: clawtilla send <agent-or-room-or-AGENT@FLEET> <message>\n");
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
        /* Another fleet's agent: it went to the outbox of that link. */
        else if (json_object_has_member(payload, "room"))
            g_print("Sent to %s, in another fleet. Its fleet decides whether "
                    "it lands; read the conversation with: clawtilla room history %s.\n",
                    argv[2], json_object_get_string_member(payload, "room"));
        else if (queued == 0)
            /*
             * Not "check the room's members", which is what this used
             * to say and is the wrong place to look almost every time.
             * In a room that requires mentions, reaching nobody is the
             * ordinary outcome: the message is in the transcript and
             * everybody can read it, and nobody was interrupted.
             */
            g_print("Posted, and nobody was interrupted -- it named no "
                    "member. Everyone in the room can read it. Write "
                    "@their-id to reach somebody, or @all for all of "
                    "them.\n");
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
        JsonArray *held;
        guint i;

        reply = call(client, kind, build_payload("agent", agent_id, NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        items = json_object_get_array_member(json_node_get_object(reply),
                                             "items");

        held = json_object_has_member(json_node_get_object(reply), "follow_ups")
            ? json_object_get_array_member(json_node_get_object(reply), "follow_ups")
            : NULL;
        for (i = 0; held != NULL && i < json_array_get_length(held); i++) {
            JsonObject *item = json_array_get_object_element(held, i);

            g_print("Queued follow-up (%s):\n%s\n\n",
                    member_or(item, "room", ""), member_or(item, "body", ""));
        }

        if (json_array_get_length(items) == 0 &&
            (held == NULL || json_array_get_length(held) == 0)) {
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

        g_print("\n%" G_GINT64_FORMAT " waiting for delivery.\n",
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
        (g_strcmp0(verb, "list") != 0 && g_strcmp0(verb, "search") != 0 &&
         g_strcmp0(verb, "journal") != 0 && g_strcmp0(verb, "undo") != 0 &&
         g_strcmp0(verb, "proposals") != 0 &&
         g_strcmp0(verb, "accept") != 0 && g_strcmp0(verb, "reject") != 0 &&
         g_strcmp0(verb, "review") != 0)) {
        g_printerr("Usage: clawtilla memory list <agent> [--category C]\n");
        g_printerr("       clawtilla memory search <agent> <query>\n");
        g_printerr("       clawtilla memory journal <agent>\n");
        g_printerr("       clawtilla memory undo <agent> <change> "
                   "[--scope agent|team|fleet]\n");
        g_printerr("       clawtilla memory proposals <agent>\n");
        g_printerr("       clawtilla memory accept|reject <agent> "
                   "<proposal>\n");
        g_printerr("       clawtilla memory review <agent>\n");
        return EXIT_FAILURE;
    }

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    /*
     * The periodic review's proposals (memories.review.model): listed,
     * and accepted or rejected one at a time.  Accepting is applied by
     * the daemon through the journaled store, so `memory journal` shows
     * it afterwards with an undo like any other change.
     */
    if (g_strcmp0(verb, "proposals") == 0) {
        JsonObject *root;
        JsonArray *rows;

        reply = call(client, "memory.proposals",
                     build_payload("agent", target, NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        root = json_node_get_object(reply);
        rows = json_object_get_array_member(root, "proposals");
        g_print("Review: %s\n", member_or(root, "model_label", "Off"));

        if (member_or(root, "last_error", NULL) != NULL)
            g_print("Last pass: %s\n", member_or(root, "last_error", ""));

        if (rows == NULL || json_array_get_length(rows) == 0) {
            g_print("No proposals waiting.\n");
            return EXIT_SUCCESS;
        }

        for (i = 0; i < json_array_get_length(rows); i++) {
            JsonObject *row = json_array_get_object_element(rows, i);
            JsonArray *about = json_object_get_array_member(row, "memories");
            guint j;

            g_print("%s  %s\n", member_or(row, "id", "?"),
                    member_or(row, "line", ""));

            if (member_or(row, "reason", NULL) != NULL)
                g_print("    why: %s\n", member_or(row, "reason", ""));

            if (member_or(row, "content", NULL) != NULL)
                g_print("    into: %s\n", member_or(row, "content", ""));

            for (j = 0; about != NULL && j < json_array_get_length(about);
                 j++) {
                JsonObject *memory = json_array_get_object_element(about, j);

                g_print("    %s: %s\n", member_or(memory, "id", "?"),
                        member_or(memory, "excerpt", "(gone)"));
            }
        }

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "accept") == 0 || g_strcmp0(verb, "reject") == 0) {
        if (argc < 5) {
            g_printerr("Usage: clawtilla memory %s <agent> <proposal>\n",
                       verb);
            return EXIT_FAILURE;
        }

        reply = call(client, g_strcmp0(verb, "accept") == 0
                     ? "memory.proposal_accept" : "memory.proposal_reject",
                     build_payload("agent", target, "id", argv[4], NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        g_print(g_strcmp0(verb, "accept") == 0
                ? "Accepted; `clawtilla memory journal %s` can undo it.\n"
                : "Rejected; nothing was changed for %s.\n", target);
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "review") == 0) {
        reply = call(client, "memory.review",
                     build_payload("agent", target, NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("Reviewing; `clawtilla memory proposals %s` lists what it "
                "suggests.\n", target);
        return EXIT_SUCCESS;
    }

    /*
     * The review: every remember, forget and pin, and an undo per row.
     * Undo is the one write this verb has, and it only ever takes back
     * a change the agent made -- it never puts words in its mouth.
     */
    if (g_strcmp0(verb, "journal") == 0) {
        JsonArray *rows;

        reply = call(client, "memory.journal",
                     build_payload("agent", target, NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        rows = json_object_get_array_member(json_node_get_object(reply),
                                               "entries");

        if (rows == NULL || json_array_get_length(rows) == 0) {
            g_print("No changes recorded.\n");
            return EXIT_SUCCESS;
        }

        for (i = 0; i < json_array_get_length(rows); i++) {
            JsonObject *entry = json_array_get_object_element(rows, i);
            gboolean undoable = json_object_has_member(entry, "undoable") &&
                json_object_get_boolean_member(entry, "undoable");

            g_print("#%" G_GINT64_FORMAT "  [%s]  %s\n",
                    json_object_get_int_member(entry, "id"),
                    member_or(entry, "scope", "agent"),
                    member_or(entry, "line", ""));

            if (undoable)
                g_print("    undo: clawtilla memory undo %s %" G_GINT64_FORMAT
                        "%s%s  (%s)\n", target,
                        json_object_get_int_member(entry, "id"),
                        g_strcmp0(member_or(entry, "scope", "agent"),
                                  "agent") != 0 ? " --scope " : "",
                        g_strcmp0(member_or(entry, "scope", "agent"),
                                  "agent") != 0
                            ? member_or(entry, "scope", "") : "",
                        clawt_memory_journal_undo_label(
                            member_or(entry, "action", NULL)));
            else if (member_or(entry, "why_not", NULL) != NULL &&
                     g_strcmp0(member_or(entry, "action", ""), "undo") != 0)
                g_print("    not undoable: %s\n",
                        member_or(entry, "why_not", ""));
        }

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "undo") == 0) {
        g_autoptr(JsonBuilder) builder = json_builder_new();
        const gchar *scope = "agent";
        int arg;

        if (argc < 5) {
            g_printerr("Usage: clawtilla memory undo <agent> <change> "
                       "[--scope agent|team|fleet]\n");
            return EXIT_FAILURE;
        }

        for (arg = 5; arg + 1 < argc; arg++)
            if (g_strcmp0(argv[arg], "--scope") == 0)
                scope = argv[arg + 1];

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "agent");
        json_builder_add_string_value(builder, target);
        json_builder_set_member_name(builder, "scope");
        json_builder_add_string_value(builder, scope);
        json_builder_set_member_name(builder, "id");
        json_builder_add_int_value(builder,
                                   g_ascii_strtoll(argv[4], NULL, 10));
        json_builder_end_object(builder);

        reply = call(client, "memory.undo", json_builder_get_root(builder));
        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("Undone.\n");
        return EXIT_SUCCESS;
    }

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

/*
 * Runs a CLI's own login against an account directory.
 *
 * The CLI is found and its account variables set by the same ai-glib
 * client libreclaw will run, so the directory logged into here is
 * exactly the one the agent reads -- not a second spelling of
 * CLAUDE_CONFIG_DIR kept in this file.  It runs on this machine: an
 * account must be logged in where the daemon's agents run.
 */
static gint
account_login(const gchar *provider, const gchar *config_dir)
{
    const gchar *cli_type = lc_provider_type_normalize(provider, "account");
    /* A provider libreclaw refuses has no CLI to log in with. */
    g_autoptr(AiCliClient) probe = (provider == NULL || cli_type != NULL)
        ? lc_client_factory_new(cli_type, NULL, NULL, NULL, FALSE) : NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *executable = NULL;
    g_auto(GStrv) envp = NULL;
    GHashTable *env;
    GHashTableIter iter;
    gpointer key;
    gpointer value;
    const gchar *argv[2] = { NULL, NULL };
    gint status = 0;

    if (probe == NULL || !ai_cli_client_set_config_dir(probe, config_dir, &error)) {
        g_printerr("clawtilla: %s\n", error != NULL ? error->message
                                                   : "no such agent CLI");
        return EXIT_FAILURE;
    }

    /* Logging in is the deliberate act that makes the directory exist. */
    if (g_mkdir_with_parents(config_dir, 0700) != 0) {
        g_printerr("clawtilla: cannot create %s: %s\n", config_dir, g_strerror(errno));
        return EXIT_FAILURE;
    }

    executable = AI_CLI_CLIENT_GET_CLASS(probe)->get_executable_path(probe);
    if (executable == NULL) {
        g_printerr("clawtilla: %s is not installed here\n", cli_type);
        return EXIT_FAILURE;
    }

    envp = g_get_environ();
    env = ai_cli_client_get_environment(probe);
    if (env != NULL) {
        g_hash_table_iter_init(&iter, env);
        while (g_hash_table_iter_next(&iter, &key, &value))
            envp = g_environ_setenv(envp, key, value, TRUE);
    }

    g_print("Starting %s with its login kept in %s.\n"
            "Log in with the CLI's own command (claude: /login; codex and grok "
            "ask on start; opencode: /connect), then quit it.\n\n",
            executable, config_dir);

    argv[0] = executable;
    if (!g_spawn_sync(NULL, (gchar **)argv, envp,
                      G_SPAWN_CHILD_INHERITS_STDIN, NULL, NULL, NULL, NULL,
                      &status, &error)) {
        g_printerr("clawtilla: %s\n", error->message);
        return EXIT_FAILURE;
    }

    return g_spawn_check_wait_status(status, NULL) ? EXIT_SUCCESS : EXIT_FAILURE;
}

/*
 * The Claude Code PreToolUse hook: one tool call the CLI is about to
 * make, judged by the agent's action rules.
 *
 * Claude writes the call as JSON on stdin; exit 0 lets it run, exit 2
 * blocks it and shows stderr to the model as the reason. Anything that
 * stops this from getting an answer -- no daemon, no token, a malformed
 * call -- blocks rather than allows: the hook exists because somebody
 * wrote a rule, and a rule that lapses when the daemon restarts is not
 * one. The daemon writes the settings that run this; nobody types it.
 */
static gint
cmd_policy_hook(int argc, char *argv[])
{
    g_autofree gchar *agent = NULL;
    g_autofree gchar *token_file = NULL;
    g_autofree gchar *socket_path = NULL;
    g_autofree gchar *token = NULL;
    g_autofree gchar *input = NULL;
    g_autofree gchar *summary = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(JsonBuilder) builder = json_builder_new();
    GOptionEntry hook_entries[] = {
        { "agent", 0, 0, G_OPTION_ARG_STRING, &agent, "Which agent", "ID" },
        { "token-file", 0, 0, G_OPTION_ARG_FILENAME, &token_file, "The agent's token", "PATH" },
        { "socket", 0, 0, G_OPTION_ARG_FILENAME, &socket_path, "The daemon's socket", "PATH" },
        { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
    };
    g_autoptr(GOptionContext) context = g_option_context_new("- judge one tool call");
    g_autoptr(GIOChannel) in = NULL;
    const gchar *tool;
    JsonObject *call;
    gint hook_argc = argc - 1;
    gchar **hook_argv = argv + 1;

    g_option_context_add_main_entries(context, hook_entries, NULL);
    if (!g_option_context_parse(context, &hook_argc, &hook_argv, &error) ||
        agent == NULL || token_file == NULL) {
        g_printerr("clawtilla policy-hook: %s\n",
                   error != NULL ? error->message : "--agent and --token-file are required");
        return 2;
    }

    if (!g_file_get_contents(token_file, &token, NULL, &error)) {
        g_printerr("Blocked: the policy hook could not read this agent's token (%s), "
                   "so it cannot ask whether this call is allowed.\n", error->message);
        return 2;
    }
    g_strstrip(token);

    in = g_io_channel_unix_new(0);
    if (g_io_channel_read_to_end(in, &input, NULL, &error) != G_IO_STATUS_NORMAL ||
        !json_parser_load_from_data(parser, input, -1, &error) ||
        !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        g_printerr("Blocked: the policy hook could not read the call: %s\n",
                   error != NULL ? error->message : "not a JSON object");
        return 2;
    }

    call = json_node_get_object(json_parser_get_root(parser));
    tool = json_object_has_member(call, "tool_name")
        ? json_object_get_string_member(call, "tool_name") : NULL;
    if (tool == NULL) {
        g_printerr("Blocked: the call named no tool.\n");
        return 2;
    }

    /* The call, for the person asked: the tool and its input, bounded. */
    if (json_object_has_member(call, "tool_input")) {
        g_autofree gchar *args = json_to_string(json_object_get_member(call, "tool_input"), FALSE);

        if (strlen(args) > 400)
            args[400] = '\0';
        summary = g_strdup_printf("%s %s", tool, args);
    } else {
        summary = g_strdup(tool);
    }

    client = clawt_client_new(socket_path);
    if (!clawt_client_connect(client, &error)) {
        g_printerr("Blocked: the policy hook could not reach the clawtilla daemon (%s), "
                   "and a rule that lapses when the daemon is away is not a rule.\n",
                   error->message);
        return 2;
    }

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "agent");
    json_builder_add_string_value(builder, agent);
    json_builder_set_member_name(builder, "token");
    json_builder_add_string_value(builder, token);
    json_builder_set_member_name(builder, "tool");
    json_builder_add_string_value(builder, tool);
    json_builder_set_member_name(builder, "summary");
    json_builder_add_string_value(builder, summary);
    if (g_getenv("CLAWTILLA_ROOM_ID") != NULL && *g_getenv("CLAWTILLA_ROOM_ID") != '\0') {
        json_builder_set_member_name(builder, "room");
        json_builder_add_string_value(builder, g_getenv("CLAWTILLA_ROOM_ID"));
    }
    json_builder_end_object(builder);

    /* As long as a person may take to answer an `ask`, and a margin. */
    reply = clawt_client_request_full(client, "policy.check", json_builder_get_root(builder),
                                      3600, &error);
    if (reply == NULL || !JSON_NODE_HOLDS_OBJECT(reply)) {
        g_printerr("Blocked: the daemon did not answer whether this call is allowed: %s\n",
                   error != NULL ? error->message : "no answer");
        return 2;
    }

    if (json_object_get_boolean_member_with_default(json_node_get_object(reply),
                                                    "allowed", FALSE))
        return 0;

    g_printerr("%s\n", json_object_get_string_member_with_default(
        json_node_get_object(reply), "reason", "Refused by an action rule."));
    return 2;
}

static gint
cmd_account(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    const gchar *verb = (argc > 2) ? argv[2] : NULL;

    if (verb == NULL) {
        g_printerr("Usage: clawtilla account <list|add|rm|login> [ARGS...]\n");
        g_printerr("\n");
        g_printerr("  list                                accounts, their CLI and directory\n");
        g_printerr("  add <id> <provider> <dir> [DESC]    declare one\n");
        g_printerr("  rm <id>                             remove it; agents using it are disabled\n");
        g_printerr("  login <id>                          run the CLI's own login against it\n");
        g_printerr("\n");
        g_printerr("Then point an agent at it:\n");
        g_printerr("  clawtilla agent set <agent> model.account <id>\n");
        return EXIT_FAILURE;
    }

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    if (g_strcmp0(verb, "list") == 0 || g_strcmp0(verb, "login") == 0) {
        JsonArray *accounts;
        guint i;

        if (g_strcmp0(verb, "login") == 0 && argc < 4) {
            g_printerr("Usage: clawtilla account login <id>\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "account.list", NULL);
        if (reply == NULL)
            return EXIT_FAILURE;
        accounts = json_object_get_array_member(json_node_get_object(reply), "accounts");

        for (i = 0; i < json_array_get_length(accounts); i++) {
            JsonObject *account = json_array_get_object_element(accounts, i);

            if (g_strcmp0(verb, "login") == 0) {
                if (g_strcmp0(member_or(account, "id", NULL), argv[3]) == 0)
                    return account_login(member_or(account, "provider", NULL),
                                         member_or(account, "config_dir", NULL));
                continue;
            }

            g_print("%-16s %-12s %s\n", member_or(account, "id", "?"),
                    member_or(account, "provider", "?"),
                    member_or(account, "config_dir", "?"));
            if (member_or(account, "description", NULL) != NULL)
                g_print("%-16s %s\n", "", member_or(account, "description", ""));
        }

        if (g_strcmp0(verb, "login") == 0) {
            g_printerr("clawtilla: no account '%s'\n", argv[3]);
            return EXIT_FAILURE;
        }
        if (json_array_get_length(accounts) == 0)
            g_print("No accounts. Agents log in as each CLI's default. Add one with:\n"
                    "  clawtilla account add work claude-code ~/.claude-work\n");
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "add") == 0) {
        if (argc < 6) {
            g_printerr("Usage: clawtilla account add <id> <provider> <dir> [DESCRIPTION]\n");
            return EXIT_FAILURE;
        }
        reply = call(client, "account.add",
                     build_payload("id", argv[3], "provider", argv[4],
                                   "config_dir", argv[5],
                                   "description", (argc > 6) ? argv[6] : NULL, NULL));
        if (reply == NULL)
            return EXIT_FAILURE;
        g_print("Added account %s. Log in to it once:\n"
                "  clawtilla account login %s\n", argv[3], argv[3]);
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "rm") == 0) {
        gint64 using;

        if (argc < 4) {
            g_printerr("Usage: clawtilla account rm <id>\n");
            return EXIT_FAILURE;
        }
        reply = call(client, "account.remove", build_payload("id", argv[3], NULL));
        if (reply == NULL)
            return EXIT_FAILURE;
        using = json_object_get_int_member(json_node_get_object(reply), "agents_using");
        g_print("Removed account %s.\n", argv[3]);
        if (using > 0)
            g_print("%" G_GINT64_FORMAT " agent%s still name%s it and %s disabled until "
                    "pointed elsewhere:\n  clawtilla agent set <agent> model.account \"\"\n",
                    using, using == 1 ? "" : "s", using == 1 ? "s" : "",
                    using == 1 ? "is" : "are");
        if (report_refusals(reply))
            return EXIT_FAILURE;
        return EXIT_SUCCESS;
    }

    g_printerr("clawtilla: unknown account command '%s'\n", verb);
    return EXIT_FAILURE;
}

/*
 * Projects: what work is for, where it lives, and what it may spend.
 * `create` takes key=value pairs after the id, so a project can be made
 * whole in one line.
 */
/* Prints a preview's items: what is new, what exists, what is dangerous. */
static void
print_fleet_preview(JsonObject *preview)
{
    JsonArray *items = json_object_get_array_member(preview, "items");
    JsonArray *to_fill = json_object_get_array_member(preview, "to_fill");
    guint i;

    for (i = 0; i < json_array_get_length(items); i++) {
        JsonObject *item = json_array_get_object_element(items, i);
        JsonArray *dangerous = json_object_get_array_member(item,
                                                            "dangerous");
        g_autoptr(GString) flags = g_string_new(NULL);
        guint d;

        for (d = 0; d < json_array_get_length(dangerous); d++)
            g_string_append_printf(flags, "%s%s", d > 0 ? ", " : "",
                                   json_array_get_string_element(dangerous,
                                                                 d));

        g_print("%-8s %-24s %-8s %s%s\n", member_or(item, "kind", ""),
                member_or(item, "id", ""),
                json_object_get_boolean_member(item, "exists")
                    ? "exists" : "new",
                flags->len > 0 ? "dangerous: " : "", flags->str);
    }

    if (json_array_get_length(to_fill) > 0)
        g_print("\nTo fill in after the import:\n");
    for (i = 0; i < json_array_get_length(to_fill); i++)
        g_print("  %s\n", json_array_get_string_element(to_fill, i));
}

/*
 * A fleet as a directory: export one, or take one in -- previewed first,
 * each collision skipped unless told to replace or rename it.
 */
static gint
cmd_fleet(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    const gchar *verb = (argc > 2) ? argv[2] : NULL;
    g_autofree gchar *dir = NULL;

    if (verb == NULL || argc < 4 || g_strcmp0(verb, "help") == 0) {
        g_print("Usage: clawtilla fleet export <dir>\n"
                "       clawtilla fleet import <dir> [--preview] "
                "[--skip KIND:ID] [--replace KIND:ID]\n"
                "                              [--rename KIND:ID=NEWID] "
                "[--allow-dangerous]\n\n"
                "A pack holds the agents, teams, routines, triggers and "
                "projects,\neach agent's persona files and the library's "
                "skills. Secrets and\nthis machine's paths are left out "
                "and listed as to fill in.\n"
                "An import adds what is new and skips what exists unless "
                "told;\nagents arrive held, skills disabled, and nothing "
                "that turns on\na dangerous setting comes in without "
                "--allow-dangerous.\n\n"
                "Examples:\n"
                "  clawtilla fleet export ~/fleet-pack\n"
                "  clawtilla fleet import ~/fleet-pack --preview\n"
                "  clawtilla fleet import ~/fleet-pack --rename "
                "agent:scout=scout2\n");
        return (verb == NULL || argc < 4) ? EXIT_FAILURE : EXIT_SUCCESS;
    }

    /* The daemon resolves paths from its own directory: made absolute. */
    {
        g_autofree gchar *expanded = clawt_expand_path(argv[3]);

        dir = g_canonicalize_filename(expanded, NULL);
    }

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    if (g_strcmp0(verb, "export") == 0) {
        JsonObject *report;
        JsonArray *to_fill;
        guint i;

        reply = call(client, "fleet.export", build_payload("dir", dir, NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        report = json_node_get_object(reply);
        g_print("Exported to %s: %" G_GINT64_FORMAT " agents, %"
                G_GINT64_FORMAT " teams, %" G_GINT64_FORMAT " routines, %"
                G_GINT64_FORMAT " triggers, %" G_GINT64_FORMAT
                " projects, %" G_GINT64_FORMAT " skills.\n", dir,
                json_object_get_int_member(report, "agents"),
                json_object_get_int_member(report, "teams"),
                json_object_get_int_member(report, "routines"),
                json_object_get_int_member(report, "triggers"),
                json_object_get_int_member(report, "projects"),
                json_object_get_int_member(report, "skills"));

        to_fill = json_object_get_array_member(report, "to_fill");
        if (json_array_get_length(to_fill) > 0)
            g_print("Left out, to fill in where it is imported:\n");
        for (i = 0; i < json_array_get_length(to_fill); i++)
            g_print("  %s\n", json_array_get_string_element(to_fill, i));
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "import") == 0) {
        g_autoptr(GString) resolutions = g_string_new(NULL);
        gboolean preview = FALSE;
        gboolean dangerous = FALSE;
        gint arg;

        for (arg = 4; arg < argc; arg++) {
            const gchar *what = NULL;

            if (g_strcmp0(argv[arg], "--preview") == 0) {
                preview = TRUE;
                continue;
            }
            if (g_strcmp0(argv[arg], "--allow-dangerous") == 0) {
                dangerous = TRUE;
                continue;
            }
            if (arg + 1 >= argc) {
                g_printerr("clawtilla: %s needs a value\n", argv[arg]);
                return EXIT_FAILURE;
            }

            if (g_strcmp0(argv[arg], "--skip") == 0)
                what = "skip";
            else if (g_strcmp0(argv[arg], "--replace") == 0)
                what = "replace";

            if (what != NULL) {
                g_string_append_printf(resolutions, "%s%s=%s",
                                       resolutions->len > 0 ? "," : "",
                                       argv[++arg], what);
            } else if (g_strcmp0(argv[arg], "--rename") == 0) {
                g_auto(GStrv) halves = g_strsplit(argv[++arg], "=", 2);

                if (g_strv_length(halves) != 2) {
                    g_printerr("clawtilla: --rename takes KIND:ID=NEWID\n");
                    return EXIT_FAILURE;
                }
                g_string_append_printf(resolutions, "%s%s=rename:%s",
                                       resolutions->len > 0 ? "," : "",
                                       halves[0], halves[1]);
            } else {
                g_printerr("clawtilla: unknown option %s\n", argv[arg]);
                return EXIT_FAILURE;
            }
        }

        if (preview) {
            reply = call(client, "fleet.import_preview",
                         build_payload("dir", dir, NULL));
            if (reply == NULL)
                return EXIT_FAILURE;
            print_fleet_preview(json_node_get_object(reply));
            return EXIT_SUCCESS;
        }

        reply = call(client, "fleet.import",
                     build_payload("dir", dir,
                                   "resolutions", resolutions->len > 0
                                       ? resolutions->str : NULL,
                                   "allow_dangerous",
                                   dangerous ? "true" : NULL, NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        {
            JsonArray *taken = json_object_get_array_member(
                json_node_get_object(reply), "taken");
            guint i;

            g_print("Taken in (agents are held: start them when they are "
                    "ready; skills are disabled until read):\n");
            for (i = 0; i < json_array_get_length(taken); i++)
                g_print("  %s\n", json_array_get_string_element(taken, i));
        }
        return EXIT_SUCCESS;
    }

    g_printerr("clawtilla: unknown fleet command '%s'\n", verb);
    return EXIT_FAILURE;
}

/*
 * Version history: every saved version of the config, the persona files
 * and the skills; compare any two, or one with now, and put one back.
 */
static gint
cmd_history(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    const gchar *verb = (argc > 2) ? argv[2] : "list";

    if (g_strcmp0(verb, "help") == 0 || g_strcmp0(verb, "--help") == 0 ||
        g_strcmp0(verb, "-h") == 0) {
        g_print("Usage: clawtilla history [list] [config|persona|skill] "
                "[SUBJECT]\n"
                "       clawtilla history show <id>\n"
                "       clawtilla history diff <id> [<newer id>]\n"
                "       clawtilla history restore <id>\n\n"
                "Subjects: `config`; `<agent>/<file>` for a persona file;\n"
                "a skill's name. A diff against one id compares it with "
                "now.\n"
                "Restoring is a save, so it becomes the newest version.\n\n"
                "Examples:\n"
                "  clawtilla history persona scout/SOUL.org\n"
                "  clawtilla history diff 41\n"
                "  clawtilla history restore 41\n");
        return EXIT_SUCCESS;
    }

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    if (g_strcmp0(verb, "show") == 0 && argc > 3) {
        reply = call(client, "revision.show",
                     build_payload("id", argv[3], NULL));
        if (reply == NULL)
            return EXIT_FAILURE;
        g_print("%s", member_or(json_node_get_object(reply), "text", ""));
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "diff") == 0 && argc > 3) {
        reply = call(client, "revision.diff",
                     build_payload("from", argv[3],
                                   "to", argc > 4 ? argv[4] : NULL, NULL));
        if (reply == NULL)
            return EXIT_FAILURE;
        g_print("%s", member_or(json_node_get_object(reply), "diff", ""));
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "restore") == 0 && argc > 3) {
        reply = call(client, "revision.restore",
                     build_payload("id", argv[3], NULL));
        if (reply == NULL)
            return EXIT_FAILURE;
        g_print("Restored revision %s; it is the newest version now.\n",
                argv[3]);
        return EXIT_SUCCESS;
    }

    /* list, with an optional kind and subject */
    {
        gint first = (g_strcmp0(verb, "list") == 0) ? 3 : 2;
        const gchar *kind = (argc > first) ? argv[first] : NULL;
        const gchar *subject = (argc > first + 1) ? argv[first + 1] : NULL;
        JsonArray *revisions;
        guint i;

        reply = call(client, "revision.list",
                     build_payload("kind", kind, "subject", subject, NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        revisions = json_object_get_array_member(json_node_get_object(reply),
                                                 "revisions");
        if (json_array_get_length(revisions) == 0)
            g_print("No versions kept yet.\n");

        for (i = 0; i < json_array_get_length(revisions); i++) {
            JsonObject *revision = json_array_get_object_element(revisions,
                                                                 i);
            g_autoptr(GDateTime) at = g_date_time_new_from_unix_local(
                json_object_get_int_member(revision, "at"));
            g_autofree gchar *when = g_date_time_format(at, "%F %R");

            g_print("%6" G_GINT64_FORMAT "  %s  %-8s %-28s %-10s %s\n",
                    json_object_get_int_member(revision, "id"), when,
                    member_or(revision, "kind", ""),
                    member_or(revision, "subject", ""),
                    member_or(revision, "author", ""),
                    member_or(revision, "note", ""));
        }
        return EXIT_SUCCESS;
    }
}

static gint
cmd_project(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    const gchar *verb = (argc > 2) ? argv[2] : NULL;

    if (verb == NULL) {
        g_printerr("Usage: clawtilla project <list|create|set|rm> "
                   "[ARGS...]\n\n"
                   "  list                        projects, and what each "
                   "spent this month\n"
                   "  create <id> [KEY=VALUE...]  add one: name, directory, "
                   "goal, review_by,\n"
                   "                              agents, monthly_budget_usd,"
                   " budget_warn_percent,\n"
                   "                              budget_hard_stop\n"
                   "  set <id> <key> <value>      change one field ("
                   "\"\" removes it)\n"
                   "  rm <id>                     remove it; its tasks keep "
                   "the name\n\n"
                   "Examples:\n"
                   "  clawtilla project create site directory=~/src/site "
                   "goal=\"Ship the new site\"\n"
                   "  clawtilla project set site monthly_budget_usd 40\n"
                   "  clawtilla task create --agent builder --project site "
                   "\"fix the header\"\n");
        return EXIT_FAILURE;
    }

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    if (g_strcmp0(verb, "list") == 0) {
        JsonArray *projects;
        guint i;

        reply = call(client, "project.list", NULL);
        if (reply == NULL)
            return EXIT_FAILURE;

        projects = json_object_get_array_member(json_node_get_object(reply),
                                                "projects");
        if (json_array_get_length(projects) == 0) {
            g_print("No projects. Add one with `clawtilla project create`."
                    "\n");
            return EXIT_SUCCESS;
        }

        for (i = 0; i < json_array_get_length(projects); i++) {
            JsonObject *project = json_array_get_object_element(projects, i);
            gdouble budget = json_object_get_double_member_with_default(
                project, "monthly_budget_usd", 0);
            g_autofree gchar *of = (budget > 0)
                ? g_strdup_printf(" of $%.2f", budget) : g_strdup("");

            g_print("%-16s $%.2f this month%s%s  %s\n",
                    member_or(project, "id", ""),
                    json_object_get_double_member_with_default(
                        project, "month_spend_usd", 0),
                    of,
                    json_object_get_boolean_member_with_default(
                        project, "stopped", FALSE) ? " (stopped)" : "",
                    member_or(project, "goal", ""));
        }
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "create") == 0 && argc > 3) {
        g_autoptr(JsonBuilder) builder = json_builder_new();
        gint arg;

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "project");
        json_builder_add_string_value(builder, argv[3]);

        for (arg = 4; arg < argc; arg++) {
            const gchar *equals = strchr(argv[arg], '=');
            g_autofree gchar *key = NULL;

            if (equals == NULL) {
                g_printerr("clawtilla: '%s' is not key=value\n", argv[arg]);
                return EXIT_FAILURE;
            }

            key = g_strndup(argv[arg], equals - argv[arg]);
            json_builder_set_member_name(builder, key);

            /* A directory is the daemon's to resolve: made absolute here. */
            if (g_strcmp0(key, "directory") == 0) {
                g_autofree gchar *expanded = clawt_expand_path(equals + 1);
                g_autofree gchar *absolute =
                    g_canonicalize_filename(expanded, NULL);

                json_builder_add_string_value(builder, absolute);
            } else {
                json_builder_add_string_value(builder, equals + 1);
            }
        }

        json_builder_end_object(builder);
        reply = call(client, "project.create", json_builder_get_root(builder));
        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("Added project %s.\n", argv[3]);
        return report_refusals(reply) ? EXIT_FAILURE : EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "set") == 0 && argc > 5) {
        reply = call(client, "project.set",
                     build_payload("project", argv[3], "key", argv[4],
                                   "value", argv[5], NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("Set %s on %s.\n", argv[4], argv[3]);
        return report_refusals(reply) ? EXIT_FAILURE : EXIT_SUCCESS;
    }

    if ((g_strcmp0(verb, "rm") == 0 || g_strcmp0(verb, "remove") == 0) &&
        argc > 3) {
        reply = call(client, "project.remove",
                     build_payload("project", argv[3], NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("Removed project %s.\n", argv[3]);
        return report_refusals(reply) ? EXIT_FAILURE : EXIT_SUCCESS;
    }

    g_printerr("clawtilla: unknown or incomplete project command '%s'\n",
               verb);
    return EXIT_FAILURE;
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
        g_printerr("  tag <list|add|remove> ...     tags that give a team a "
                   "job (docs/team-tags.org)\n");
        return EXIT_FAILURE;
    }

    /* the well-known table needs no daemon: it is this build's */
    if (g_strcmp0(verb, "tag") == 0 && argc > 4 && g_strcmp0(argv[3], "list") == 0 &&
        g_strcmp0(argv[4], "--well-known") == 0) {
        guint i;

        for (i = 0; i < clawt_team_tag_count(); i++) {
            const ClawtTeamTagInfo *info = clawt_team_tag_nth(i);

            g_print("%-20s %s%s\n", info->name, info->unique ? "[one team] " : "",
                    info->description);
        }
        return EXIT_SUCCESS;
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

    if (g_strcmp0(verb, "tag") == 0) {
        const gchar *action = argc > 3 ? argv[3] : NULL;
        JsonArray *teams;
        guint i;

        if (g_strcmp0(action, "list") != 0 && g_strcmp0(action, "add") != 0 &&
            g_strcmp0(action, "remove") != 0) {
            g_printerr("Usage: clawtilla team tag list [--well-known]\n"
                       "       clawtilla team tag add <team> <tag>\n"
                       "       clawtilla team tag remove <team> <tag>\n"
                       "\n"
                       "Examples:\n"
                       "  clawtilla team tag add interns ephemeral\n"
                       "  clawtilla team tag list --well-known\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "team.list", NULL);
        if (reply == NULL)
            return EXIT_FAILURE;
        teams = json_object_get_array_member(json_node_get_object(reply), "teams");

        if (g_strcmp0(action, "list") == 0) {
            for (i = 0; i < json_array_get_length(teams); i++) {
                JsonObject *team = json_array_get_object_element(teams, i);
                JsonArray *tags = json_object_has_member(team, "tags")
                    ? json_object_get_array_member(team, "tags") : NULL;
                GString *line = g_string_new(NULL);
                guint t;

                for (t = 0; tags != NULL && t < json_array_get_length(tags); t++) {
                    const gchar *tag = json_array_get_string_element(tags, t);

                    g_string_append_printf(line, "%s%s%s", t > 0 ? ", " : "", tag,
                                           clawt_team_tag_lookup(tag) != NULL
                                               ? "" : " (yours)");
                }
                g_print("%-16s %s\n", member_or(team, "id", "?"),
                        line->len > 0 ? line->str : "-");
                g_string_free(line, TRUE);
            }
            return EXIT_SUCCESS;
        }

        if (argc < 6) {
            g_printerr("Usage: clawtilla team tag %s <team> <tag>\n", action);
            return EXIT_FAILURE;
        }

        /* the team's tags as they are, then the one change, sent whole */
        {
            g_autoptr(GPtrArray) next = g_ptr_array_new_with_free_func(g_free);
            g_autoptr(JsonBuilder) builder = json_builder_new();
            g_autoptr(JsonNode) set_reply = NULL;
            gboolean found = FALSE;
            gboolean had = FALSE;

            for (i = 0; i < json_array_get_length(teams); i++) {
                JsonObject *team = json_array_get_object_element(teams, i);
                JsonArray *tags;
                guint t;

                if (g_strcmp0(member_or(team, "id", NULL), argv[4]) != 0)
                    continue;
                found = TRUE;
                tags = json_object_has_member(team, "tags")
                    ? json_object_get_array_member(team, "tags") : NULL;
                for (t = 0; tags != NULL && t < json_array_get_length(tags); t++) {
                    const gchar *tag = json_array_get_string_element(tags, t);

                    if (g_strcmp0(tag, argv[5]) == 0) {
                        had = TRUE;
                        if (g_strcmp0(action, "remove") == 0)
                            continue;
                    }
                    g_ptr_array_add(next, g_strdup(tag));
                }
            }
            if (!found) {
                g_printerr("clawtilla: no team '%s'\n", argv[4]);
                return EXIT_FAILURE;
            }
            if (g_strcmp0(action, "add") == 0 && had) {
                g_print("%s already has the tag %s.\n", argv[4], argv[5]);
                return EXIT_SUCCESS;
            }
            if (g_strcmp0(action, "remove") == 0 && !had) {
                g_print("%s does not have the tag %s.\n", argv[4], argv[5]);
                return EXIT_SUCCESS;
            }
            if (g_strcmp0(action, "add") == 0)
                g_ptr_array_add(next, g_strdup(argv[5]));

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "team");
            json_builder_add_string_value(builder, argv[4]);
            json_builder_set_member_name(builder, "key");
            json_builder_add_string_value(builder, "tags");
            json_builder_set_member_name(builder, "values");
            json_builder_begin_array(builder);
            for (i = 0; i < next->len; i++)
                json_builder_add_string_value(builder, g_ptr_array_index(next, i));
            json_builder_end_array(builder);
            json_builder_end_object(builder);

            set_reply = call(client, "team.set", json_builder_get_root(builder));
            if (set_reply == NULL)
                return EXIT_FAILURE;
            g_print("%s %s the tag %s.\n", argv[4],
                    g_strcmp0(action, "add") == 0 ? "now has" : "no longer has",
                    argv[5]);
        }
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

/*
 * The daemon's notification list, as every client draws it: what is
 * unread, decisions and Grok jobs waiting, then the alerts newest first.
 * docs/clients.org, "Notifications".
 */
static gint
notices_list(ClawtClient *client, const gchar *agent, gboolean all)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonNode) reply = NULL;
    JsonObject *root;
    JsonObject *sources;
    JsonArray *notices;
    JsonArray *mutes;
    guint i;

    json_builder_begin_object(builder);

    if (agent != NULL) {
        json_builder_set_member_name(builder, "agent");
        json_builder_add_string_value(builder, agent);
    }

    json_builder_set_member_name(builder, "include_dismissed");
    json_builder_add_boolean_value(builder, all);
    json_builder_end_object(builder);

    reply = call(client, "notice.list", json_builder_get_root(builder));
    if (reply == NULL)
        return EXIT_FAILURE;

    root = json_node_get_object(reply);
    sources = json_object_get_object_member(root, "sources");

    g_print("Unread chats: %" G_GINT64_FORMAT
            "  Decisions waiting: %" G_GINT64_FORMAT
            "  Grok jobs needing attention: %" G_GINT64_FORMAT
            "  Unread alerts: %" G_GINT64_FORMAT "\n",
            json_object_get_int_member(sources, "chats_unread"),
            json_object_get_int_member(sources, "decisions_pending"),
            json_object_get_int_member(sources, "grok_attention"),
            json_object_get_int_member(root, "unread"));

    mutes = json_object_get_array_member(root, "mutes");

    for (i = 0; mutes != NULL && i < json_array_get_length(mutes); i++) {
        JsonObject *mute = json_array_get_object_element(mutes, i);
        gint64 until = json_object_get_int_member(mute, "until");

        if (until > 0) {
            g_autoptr(GDateTime) when = g_date_time_new_from_unix_local(until);
            g_autofree gchar *text = g_date_time_format(when, "%Y-%m-%d %H:%M");

            g_print("Muted: %s until %s\n", member_or(mute, "agent", "?"),
                    text);
        } else {
            g_print("Muted: %s until unmuted\n",
                    member_or(mute, "agent", "?"));
        }
    }

    notices = json_object_get_array_member(root, "notices");

    if (notices == NULL || json_array_get_length(notices) == 0) {
        g_print("No alerts.\n");
        return EXIT_SUCCESS;
    }

    for (i = 0; i < json_array_get_length(notices); i++) {
        JsonObject *notice = json_array_get_object_element(notices, i);
        g_autoptr(GDateTime) when = g_date_time_new_from_unix_local(
            json_object_get_int_member(notice, "created_at"));
        g_autofree gchar *stamp = g_date_time_format(when, "%m-%d %H:%M");
        const gchar *mark = json_object_get_boolean_member(notice, "dismissed")
                            ? "x"
                            : json_object_get_boolean_member(notice, "read")
                              ? " " : "*";

        g_print("%s %5" G_GINT64_FORMAT "  %s  %-6s  %s\n", mark,
                json_object_get_int_member(notice, "id"), stamp,
                member_or(notice, "tier", "?"),
                member_or(notice, "title", ""));
    }

    return EXIT_SUCCESS;
}

static gint
cmd_notices(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    const gchar *verb = (argc > 2 && argv[2][0] != '-') ? argv[2] : NULL;
    const gchar *usage =
        "Usage: clawtilla notices [--agent AGENT] [--all]\n"
        "       clawtilla notices read <ID|--all>\n"
        "       clawtilla notices dismiss <ID>\n"
        "       clawtilla notices clear [--agent AGENT]\n"
        "\n"
        "Examples:\n"
        "  clawtilla notices                 what wants you, and the alerts\n"
        "  clawtilla notices --agent scout   only scout's\n"
        "  clawtilla notices read --all      the bell to zero\n"
        "  clawtilla notices dismiss 42      hide one alert\n"
        "  clawtilla notices clear           hide every alert\n";
    const gchar *agent = NULL;
    gboolean all = FALSE;
    gint i;

    if (argc > 2 && (g_strcmp0(argv[2], "-h") == 0 ||
                     g_strcmp0(argv[2], "--help") == 0)) {
        g_print("%s", usage);
        return EXIT_SUCCESS;
    }

    /* --agent and --all, wherever they are after the verb. */
    for (i = (verb != NULL) ? 3 : 2; i < argc; i++) {
        if (g_strcmp0(argv[i], "--agent") == 0 && i + 1 < argc)
            agent = argv[++i];
        else if (g_strcmp0(argv[i], "--all") == 0)
            all = TRUE;
        else if (verb == NULL || argv[i][0] == '-') {
            g_printerr("%s", usage);
            return EXIT_FAILURE;
        }
    }

    /*
     * Every verb's arguments are checked before the daemon is reached,
     * so a typo is a usage message rather than a connection error.
     */
    if ((g_strcmp0(verb, "read") == 0 && !all && argc != 4) ||
        (g_strcmp0(verb, "dismiss") == 0 && argc != 4)) {
        g_printerr("%s", usage);
        return EXIT_FAILURE;
    }

    if (verb != NULL && g_strcmp0(verb, "list") != 0 &&
        g_strcmp0(verb, "read") != 0 && g_strcmp0(verb, "dismiss") != 0 &&
        g_strcmp0(verb, "clear") != 0) {
        g_printerr("clawtilla: unknown notices verb '%s'\n%s", verb, usage);
        return EXIT_FAILURE;
    }

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    if (verb == NULL || g_strcmp0(verb, "list") == 0)
        return notices_list(client, agent, all);

    if (g_strcmp0(verb, "read") == 0) {
        if (all)
            reply = call(client, "notice.read",
                         build_payload("all", "true", NULL));
        else
            reply = call(client, "notice.read",
                         build_payload("id", argv[3], NULL));

        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("Marked %" G_GINT64_FORMAT " notice(s) read.\n",
                json_object_get_int_member(json_node_get_object(reply),
                                           "read"));
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "dismiss") == 0) {
        reply = call(client, "notice.dismiss",
                     build_payload("id", argv[3], NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("Dismissed %s.\n", argv[3]);
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "clear") == 0) {
        reply = call(client, "notice.clear",
                     build_payload("agent", agent, NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("Cleared %" G_GINT64_FORMAT " notice(s).\n",
                json_object_get_int_member(json_node_get_object(reply),
                                           "cleared"));
        return EXIT_SUCCESS;
    }

    return EXIT_FAILURE;
}

/*
 * Mutes an agent's notifications everywhere -- every client's bell and
 * tab totals, and the push and desktop notifiers -- until unmuted, or
 * for as long as --for says.
 */
static gint
cmd_mute(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    gboolean mute = g_strcmp0(argv[1], "mute") == 0;
    g_autofree gchar *until = NULL;
    const gchar *usage = mute
        ? "Usage: clawtilla mute <agent> [--for DURATION]\n"
          "\n"
          "DURATION is 90s, 30m, 2h or 3d; a bare number is minutes.\n"
          "\n"
          "Examples:\n"
          "  clawtilla mute scout            until unmuted\n"
          "  clawtilla mute scout --for 2h   for two hours\n"
        : "Usage: clawtilla unmute <agent>\n"
          "\n"
          "Example:\n"
          "  clawtilla unmute scout\n";

    if (argc > 2 && (g_strcmp0(argv[2], "-h") == 0 ||
                     g_strcmp0(argv[2], "--help") == 0)) {
        g_print("%s", usage);
        return EXIT_SUCCESS;
    }

    if (argc < 3 || argv[2][0] == '-' ||
        (mute && argc != 3 && argc != 5) || (!mute && argc != 3)) {
        g_printerr("%s", usage);
        return EXIT_FAILURE;
    }

    if (mute && argc == 5) {
        gint64 seconds = 0;

        if (g_strcmp0(argv[3], "--for") != 0 ||
            !clawt_notice_mute_for_parse(argv[4], &seconds)) {
            g_printerr("%s", usage);
            return EXIT_FAILURE;
        }

        until = g_strdup_printf("%" G_GINT64_FORMAT,
                                g_get_real_time() / G_USEC_PER_SEC + seconds);
    }

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    reply = call(client, mute ? "notice.mute" : "notice.unmute",
                 build_payload("agent", argv[2], "until", until, NULL));
    if (reply == NULL)
        return EXIT_FAILURE;

    if (!mute)
        g_print("%s is no longer muted.\n", argv[2]);
    else if (until != NULL)
        g_print("%s is muted for %s.\n", argv[2], argv[4]);
    else
        g_print("%s is muted until unmuted.\n", argv[2]);

    return EXIT_SUCCESS;
}

static gint
cmd_room(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    const gchar *verb = (argc > 2) ? argv[2] : NULL;

    if (verb == NULL) {
        g_printerr("Usage: clawtilla room "
                   "<list|create|add|history|read|share|unshare> [ARGS...]\n");
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

    /* A declared room, shared with a linked fleet (0.70.0). */
    if (g_strcmp0(verb, "share") == 0 || g_strcmp0(verb, "unshare") == 0) {
        gboolean share = g_strcmp0(verb, "share") == 0;

        if (argc < 5) {
            g_printerr("Usage: clawtilla room %s <room> <fleet-alias>\n", verb);
            return EXIT_FAILURE;
        }
        reply = call(client, share ? "room.share" : "room.unshare",
                     build_payload("room", argv[3], "alias", argv[4], NULL));
        if (reply == NULL)
            return EXIT_FAILURE;
        g_print(share ? "Shared %s with %s: everything said in it goes there, and what they "
                        "write lands here.\n"
                      : "%s is no longer shared with %s.\n", argv[3], argv[4]);
        return EXIT_SUCCESS;
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

    /*
     * Marks a conversation read -- the same `room.read` a client sends
     * when it is opened -- so the count clears in every client at once.
     * An agent's id names the operator's conversation with it; the
     * daemon resolves it, so this never has to know how a direct room
     * is spelled.
     */
    if (g_strcmp0(verb, "read") == 0) {
        gboolean all = (argc == 4 && g_strcmp0(argv[3], "--all") == 0);

        if (argc != 4) {
            g_printerr("Usage: clawtilla room read <agent|room>\n"
                       "       clawtilla room read --all\n");
            return EXIT_FAILURE;
        }

        if (all) {
            g_autoptr(JsonBuilder) builder = json_builder_new();

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "all");
            json_builder_add_boolean_value(builder, TRUE);
            json_builder_end_object(builder);

            reply = call(client, "room.read", json_builder_get_root(builder));
            if (reply == NULL)
                return EXIT_FAILURE;

            g_print("Marked %" G_GINT64_FORMAT " conversation(s) read.\n",
                    json_object_has_member(json_node_get_object(reply),
                                           "marked")
                        ? json_object_get_int_member(
                              json_node_get_object(reply), "marked")
                        : 0);
            return EXIT_SUCCESS;
        }

        reply = call(client, "room.read",
                     build_payload("room", argv[3], NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("%s is read.\n",
                member_or(json_node_get_object(reply), "room", argv[3]));
        return EXIT_SUCCESS;
    }

    g_printerr("clawtilla: unknown room verb '%s'\n", verb);
    return EXIT_FAILURE;
}


/* Whether any login in a usage.providers reply is still being read. */
/*
 * clawtilla office -- who is where, the world packs, the layout.
 *
 * `who` is the roster every client draws, with the same line
 * (clawt_office_roster_line()); `worlds` lists the packs and why any was
 * refused; `layout` is the director's seats; `choreography` prints the
 * built-in rules, which are also the format an override is written in,
 * and needs no daemon.  `open` starts the renderer, when it was built.
 */
static gchar *read_secret(const gchar *prompt);

/*
 * clawtilla federation -- this fleet, linked with others.
 *
 * Every verb is one `federation.*` request; the daemon owns the keys,
 * the peer list and every connection.  The waits that reach another
 * fleet (accept, link, ping) use a longer timeout than a local request,
 * because the far side may be a tunnel away.
 */
#define FED_WAIT_SECONDS (60)

static JsonNode *
fed_call(ClawtClient *client, const gchar *kind, JsonNode *payload, gint wait)
{
    g_autoptr(GError) error = NULL;
    JsonNode *reply = clawt_client_request_full(client, kind, payload, wait, &error);

    if (reply == NULL)
        g_printerr("clawtilla: %s\n", error->message);
    return reply;
}

static void
fed_print_json(JsonNode *reply)
{
    g_autofree gchar *text = json_to_string(reply, TRUE);

    g_print("%s\n", text);
}

static gboolean
fed_has_flag(int argc, char *argv[], const gchar *flag)
{
    gint i;

    for (i = 3; i < argc; i++)
        if (g_strcmp0(argv[i], flag) == 0)
            return TRUE;
    return FALSE;
}

static const gchar *
fed_option(int argc, char *argv[], const gchar *flag)
{
    gint i;

    for (i = 3; i + 1 < argc; i++)
        if (g_strcmp0(argv[i], flag) == 0)
            return argv[i + 1];
    return NULL;
}

/* The first argument after the verb that is not an option or its value. */
static const gchar *
fed_positional(int argc, char *argv[], guint nth)
{
    static const gchar *takes_value[] = {
        "--tier", "--expires", "--note", "--endpoint", "--access-id", "--alias",
        "--tag", "--filter", "--label", "--owner", "--on-revoke", "--limit",
        "--hostname", "--token-file", NULL
    };
    guint seen = 0;
    gint i;

    for (i = 3; i < argc; i++) {
        if (g_str_has_prefix(argv[i], "--")) {
            if (g_strv_contains(takes_value, argv[i]))
                i++;
            continue;
        }
        if (seen++ == nth)
            return argv[i];
    }
    return NULL;
}

/* One line per peer: dot, alias, tier, health, owner, tags. */
static void
fed_print_peer_line(JsonObject *p)
{
    JsonArray *tags = json_object_get_array_member(p, "tags");
    g_autoptr(GString) tag_text = g_string_new(NULL);
    const gchar *health = member_or(p, "health", "?");
    const gchar *dot = g_strcmp0(health, "live") == 0 ? "●"
                     : g_strcmp0(health, "offline") == 0 ? "○"
                     : g_strcmp0(health, "revoked") == 0 ? "✕" : "◌";
    guint i;

    for (i = 0; tags != NULL && i < json_array_get_length(tags); i++)
        g_string_append_printf(tag_text, "%s#%s", i > 0 ? " " : "",
                               json_array_get_string_element(tags, i));
    g_print("%s %-16s %-10s %-11s %-18s %s%s%s\n", dot, member_or(p, "alias", "?"),
            member_or(p, "tier", "?"), health, member_or(p, "owner", ""),
            tag_text->str,
            json_object_get_boolean_member_with_default(p, "hidden", FALSE) ? "  (hidden)" : "",
            json_object_get_boolean_member_with_default(p, "muted", FALSE) ? "  (muted)" : "");
}

static void
fed_print_show(JsonObject *o)
{
    JsonArray *grant = json_object_get_array_member(o, "grant");
    JsonArray *problems = json_object_get_array_member(o, "problems");
    guint i;

    fed_print_peer_line(o);
    g_print("  fleet        %s\n", member_or(o, "fleet_id", "?"));
    g_print("  words        %s\n", member_or(o, "safety_words", "?"));
    g_print("  label        %s\n", member_or(o, "label", ""));
    g_print("  endpoint     %s (%s%s%s)\n", member_or(o, "endpoint", "(it dials this fleet)"),
            member_or(o, "transport", "?"),
            json_object_has_member(o, "path") ? ", " : "", member_or(o, "path", ""));
    g_print("  status       %s\n", member_or(o, "status", "?"));
    if (json_object_has_member(o, "latency_ms"))
        g_print("  latency      %" G_GINT64_FORMAT " ms\n",
                json_object_get_int_member(o, "latency_ms"));
    if (json_object_has_member(o, "error"))
        g_print("  last error   %s\n", member_or(o, "error", ""));
    if (json_object_has_member(o, "version"))
        g_print("  version      %s\n", member_or(o, "version", ""));
    if (grant != NULL) {
        g_print("\n  What it may do here:\n");
        for (i = 0; i < json_array_get_length(grant); i++) {
            JsonObject *f = json_array_get_object_element(grant, i);
            g_print("    %-22s %-12s %s%s\n", member_or(f, "name", "?"), member_or(f, "value", ""),
                    json_object_get_boolean_member_with_default(f, "overridden", FALSE) ? "* " : "",
                    member_or(f, "source", ""));
        }
    }
    if (json_object_has_member(o, "their_grant")) {
        JsonObject *theirs = json_object_get_object_member(o, "their_grant");
        g_print("\n  What it lets this fleet do there: reach %s, message %s, delegate %s\n",
                member_or(theirs, "reach", "?"), member_or(theirs, "message", "?"),
                member_or(theirs, "delegate", "?"));
    }
    for (i = 0; problems != NULL && i < json_array_get_length(problems); i++)
        g_print("  ! %s\n", json_array_get_string_element(problems, i));
}

static gint
cmd_federation(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    const gchar *verb = argc > 2 ? argv[2] : NULL;
    gboolean json = fed_has_flag(argc, argv, "--json");
    const gchar *alias = fed_positional(argc, argv, 0);
    JsonObject *o;
    guint i;

    if (verb == NULL || g_strcmp0(verb, "-h") == 0 || g_strcmp0(verb, "--help") == 0) {
        g_print("Usage: clawtilla federation <verb> [...]\n"
                "\n"
                "Link this fleet with other clawtilla fleets -- yours on another machine,\n"
                "or a colleague's -- over a Cloudflare tunnel (https://), a tailnet or LAN\n"
                "(http:// or clawtilla://, the daemon's own IPC port), or a socket on this\n"
                "machine.  Every request between fleets is signed by the fleet's key.\n"
                "Each linked fleet has a tier: owned, trusted, limited or untrusted.\n"
                "\n"
                "  status                     this fleet's id, safety words and address\n"
                "  tiers                      what each tier allows, by default\n"
                "  invite [--tier T] [--expires HOURS] [--confirm] [--note TEXT]\n"
                "         [--endpoint URL] [--access-id ID] [--qr]\n"
                "                             make a one-use invite (T is what the\n"
                "                             accepting fleet gets here; default limited)\n"
                "  inspect INVITE             what an invite says, without using it\n"
                "  accept INVITE --tier T [--alias A] [--tag TAG]...\n"
                "                             link with the fleet that made INVITE; T is\n"
                "                             what it gets here\n"
                "  link TARGET [--alias A] [--tag TAG]...\n"
                "                             link with another daemon you own: TARGET is\n"
                "                             its socket path, or HOST:PORT (asks for its\n"
                "                             owner token); both sides get tier owned\n"
                "  list [--filter SELECTOR]   linked fleets: health, tier, owner, tags\n"
                "  show ALIAS                 one fleet: address, grant and where each\n"
                "                             value came from (explain is the same)\n"
                "  set ALIAS [--tier T] [--label L] [--owner O] [--endpoint URL]\n"
                "            [--on-revoke cancel|finish] [FIELD=VALUE]...\n"
                "                             change a fleet; FIELD=VALUE overrides its\n"
                "                             grant (FIELD= removes the override)\n"
                "  tag ALIAS [+TAG|-TAG]...   add or remove tags\n"
                "  hide|unhide ALIAS          keep it out of every sidebar\n"
                "  mute|unmute ALIAS          quiet its alerts\n"
                "  approve ALIAS              let in a fleet that accepted a --confirm invite\n"
                "  ping ALIAS                 check the link, now\n"
                "  revoke ALIAS               end the link (and tell the other fleet)\n"
                "  forget ALIAS               remove a revoked fleet's record\n"
                "  invites | uninvite ID      outstanding invites; withdraw one\n"
                "  log [ALIAS] [--limit N]    every request between fleets, newest first\n"
                "  transcript ALIAS (--task ID | --agent AGENT) [--limit N]\n"
                "                             read back work you gave that fleet (by\n"
                "                             your task id), or its conversations with\n"
                "                             an agent, as far as its grant to you lets\n"
                "  rotate-key                 a new fleet key; linked fleets are told\n"
                "  expose --cloudflare --hostname HOST\n"
                "                             print a cloudflared config for this fleet\n"
                "\n"
                "Selectors (list --filter, federation.rules): tier:T, tag:T, owner:GLOB,\n"
                "name:GLOB, health:H, online; !x negates, a+b is and, a,b is or.\n"
                "\n"
                "Examples:\n"
                "  clawtilla federation link /run/user/1000/clawtilla/other.sock\n"
                "  clawtilla federation link 100.64.0.7:8792 --alias homelab\n"
                "  clawtilla federation invite --tier trusted --qr\n"
                "  clawtilla federation accept clawt-fed1-... --tier limited --tag client-acme\n"
                "  clawtilla federation list --filter 'tier:trusted+online'\n"
                "  clawtilla federation set acme cap.exec=deny reach=listed agents=atlas,team:web\n"
                "  clawtilla federation tag acme +billing -trial\n"
                "  clawtilla federation expose --cloudflare --hostname fleet.example.com\n");
        return verb == NULL ? EXIT_FAILURE : EXIT_SUCCESS;
    }

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    if (g_strcmp0(verb, "status") == 0) {
        reply = fed_call(client, "federation.status", NULL, 0);
        if (reply == NULL)
            return EXIT_FAILURE;
        if (json) {
            fed_print_json(reply);
            return EXIT_SUCCESS;
        }
        o = json_node_get_object(reply);
        if (!json_object_get_boolean_member_with_default(o, "enabled", FALSE)) {
            g_print("Federation is off.  %s\n", member_or(o, "hint", ""));
            return EXIT_SUCCESS;
        }
        g_print("This fleet  %s (%s)\n", member_or(o, "name", "?"), member_or(o, "owner", ""));
        g_print("Fleet id    %s\n", member_or(o, "fleet_id", "?"));
        g_print("Words       %s   (read these to whoever accepts your invite)\n",
                member_or(o, "safety_words", "?"));
        g_print("Reachable   %s\n", member_or(o, "endpoint",
                "nowhere yet -- set federation.public_url, or join a tailnet"));
        g_print("Linked      %" G_GINT64_FORMAT " fleet(s), %" G_GINT64_FORMAT " open invite(s)\n",
                json_object_get_int_member_with_default(o, "peers", 0),
                json_object_get_int_member_with_default(o, "invites", 0));
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "tiers") == 0) {
        JsonArray *tiers;
        reply = fed_call(client, "federation.tiers", NULL, 0);
        if (reply == NULL)
            return EXIT_FAILURE;
        if (json) {
            fed_print_json(reply);
            return EXIT_SUCCESS;
        }
        tiers = json_object_get_array_member(json_node_get_object(reply), "tiers");
        for (i = 0; tiers != NULL && i < json_array_get_length(tiers); i++) {
            JsonObject *t = json_array_get_object_element(tiers, i);
            g_print("%-10s %s\n", member_or(t, "tier", "?"), member_or(t, "summary", ""));
        }
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "invite") == 0) {
        g_autoptr(JsonBuilder) b = json_builder_new();
        const gchar *expires = fed_option(argc, argv, "--expires");
        const gchar *access_id = fed_option(argc, argv, "--access-id");
        g_autofree gchar *access_secret = NULL;

        json_builder_begin_object(b);
#define OPT(flag, key) do { const gchar *v_ = fed_option(argc, argv, flag); \
            if (v_ != NULL) { json_builder_set_member_name(b, key); \
                              json_builder_add_string_value(b, v_); } } while (0)
        OPT("--tier", "tier");
        OPT("--note", "note");
        OPT("--endpoint", "endpoint");
#undef OPT
        if (expires != NULL) {
            json_builder_set_member_name(b, "expires_hours");
            json_builder_add_int_value(b, g_ascii_strtoll(expires, NULL, 10));
        }
        if (fed_has_flag(argc, argv, "--confirm")) {
            json_builder_set_member_name(b, "confirm");
            json_builder_add_boolean_value(b, TRUE);
        }
        if (access_id != NULL) {
            /* The secret is typed, never an argument: argv is world-readable. */
            access_secret = read_secret("Cloudflare Access client secret: ");
            if (access_secret == NULL || *access_secret == '\0') {
                g_printerr("clawtilla: no secret was given\n");
                return EXIT_FAILURE;
            }
            json_builder_set_member_name(b, "access_id");
            json_builder_add_string_value(b, access_id);
            json_builder_set_member_name(b, "access_secret");
            json_builder_add_string_value(b, access_secret);
        }
        json_builder_end_object(b);
        reply = fed_call(client, "federation.invite.create", json_builder_get_root(b), 0);
        if (access_secret != NULL)
            memset(access_secret, 0, strlen(access_secret));
        if (reply == NULL)
            return EXIT_FAILURE;
        if (json) {
            fed_print_json(reply);
            return EXIT_SUCCESS;
        }
        o = json_node_get_object(reply);
        if (fed_has_flag(argc, argv, "--qr")) {
            g_autofree gchar *qr = clawt_qr_render_terminal(member_or(o, "uri", ""), FALSE, NULL);
            if (qr != NULL)
                g_print("%s\n", qr);
        }
        g_print("Send this to the other fleet's owner (it is a one-use secret):\n\n  %s\n\n",
                member_or(o, "paste", "?"));
        g_print("They will see this fleet as %s, words %s.\n",
                member_or(o, "fleet_id", "?"), member_or(o, "safety_words", "?"));
        g_print("It reaches this fleet at %s, gives them tier %s here, and expires in %s hours.\n",
                member_or(o, "endpoint", "?"), member_or(o, "tier", "?"),
                expires != NULL ? expires : "72");
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "inspect") == 0 || g_strcmp0(verb, "accept") == 0) {
        g_autoptr(JsonBuilder) b = json_builder_new();
        const gchar *invite = alias;
        gint k;

        if (invite == NULL) {
            g_printerr("Usage: clawtilla federation %s INVITE%s\n", verb,
                       g_strcmp0(verb, "accept") == 0 ? " --tier T" : "");
            return EXIT_FAILURE;
        }
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "invite");
        json_builder_add_string_value(b, invite);
        if (g_strcmp0(verb, "accept") == 0) {
            const gchar *tier = fed_option(argc, argv, "--tier");
            const gchar *a = fed_option(argc, argv, "--alias");
            if (tier == NULL) {
                g_printerr("clawtilla: choose the tier that fleet gets here: --tier owned|"
                           "trusted|limited|untrusted\n");
                return EXIT_FAILURE;
            }
            json_builder_set_member_name(b, "tier");
            json_builder_add_string_value(b, tier);
            if (a != NULL) {
                json_builder_set_member_name(b, "alias");
                json_builder_add_string_value(b, a);
            }
            json_builder_set_member_name(b, "tags");
            json_builder_begin_array(b);
            for (k = 3; k + 1 < argc; k++)
                if (g_strcmp0(argv[k], "--tag") == 0)
                    json_builder_add_string_value(b, argv[k + 1]);
            json_builder_end_array(b);
        }
        json_builder_end_object(b);
        reply = fed_call(client, g_strcmp0(verb, "accept") == 0 ? "federation.invite.accept"
                                                                : "federation.invite.inspect",
                         json_builder_get_root(b), FED_WAIT_SECONDS);
        if (reply == NULL)
            return EXIT_FAILURE;
        if (json) {
            fed_print_json(reply);
            return EXIT_SUCCESS;
        }
        o = json_node_get_object(reply);
        if (g_strcmp0(verb, "inspect") == 0) {
            g_print("Fleet       %s (%s)\n", member_or(o, "name", "?"), member_or(o, "owner", ""));
            g_print("Fleet id    %s\n", member_or(o, "fleet_id", "?"));
            g_print("Words       %s   (check them with its owner)\n",
                    member_or(o, "safety_words", "?"));
            g_print("Reached at  %s (%s)\n", member_or(o, "endpoint", "?"),
                    member_or(o, "path", member_or(o, "problem", "?")));
            g_print("Gives you   %s: %s\n", member_or(o, "tier", "?"),
                    member_or(o, "tier_summary", ""));
            g_print("Expires     %s\n",
                    json_object_get_boolean_member_with_default(o, "expired", FALSE)
                        ? "already expired" : "not yet");
            if (json_object_has_member(o, "already_linked_as"))
                g_print("Already linked here as %s.\n", member_or(o, "already_linked_as", ""));
            return EXIT_SUCCESS;
        }
        g_print("Linked with %s as '%s' (tier %s here; %s).\n", member_or(o, "label", "?"),
                member_or(o, "alias", "?"), member_or(o, "tier", "?"),
                g_strcmp0(member_or(o, "their_status", ""), "pending") == 0
                    ? "waiting for them to approve"
                    : "active");
        g_print("Its safety words are %s -- check them with its owner.\n",
                member_or(o, "safety_words", "?"));
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "link") == 0) {
        g_autoptr(JsonBuilder) b = json_builder_new();
        g_autofree gchar *token = NULL;
        const gchar *target = alias;
        const gchar *a = fed_option(argc, argv, "--alias");
        gint k;

        if (target == NULL) {
            g_printerr("Usage: clawtilla federation link SOCKET|HOST:PORT [--alias A]\n");
            return EXIT_FAILURE;
        }
        if (target[0] != '/') {
            token = read_secret("That daemon's owner token (clawtilla daemon token, there): ");
            if (token == NULL || *token == '\0') {
                g_printerr("clawtilla: no token was given\n");
                return EXIT_FAILURE;
            }
        }
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "target");
        json_builder_add_string_value(b, target);
        if (token != NULL) {
            json_builder_set_member_name(b, "token");
            json_builder_add_string_value(b, token);
        }
        if (a != NULL) {
            json_builder_set_member_name(b, "alias");
            json_builder_add_string_value(b, a);
        }
        json_builder_set_member_name(b, "tags");
        json_builder_begin_array(b);
        for (k = 3; k + 1 < argc; k++)
            if (g_strcmp0(argv[k], "--tag") == 0)
                json_builder_add_string_value(b, argv[k + 1]);
        json_builder_end_array(b);
        json_builder_end_object(b);
        reply = fed_call(client, "federation.link", json_builder_get_root(b), FED_WAIT_SECONDS);
        if (token != NULL)
            memset(token, 0, strlen(token));
        if (reply == NULL)
            return EXIT_FAILURE;
        o = json_node_get_object(reply);
        g_print("Linked with %s as '%s', tier owned on both sides.\n",
                member_or(o, "label", "?"), member_or(o, "alias", "?"));
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "list") == 0 || g_strcmp0(verb, "invites") == 0) {
        JsonArray *list;
        reply = fed_call(client, "federation.peer.list",
                         build_payload("filter", fed_option(argc, argv, "--filter"), NULL), 0);
        if (reply == NULL)
            return EXIT_FAILURE;
        if (json) {
            fed_print_json(reply);
            return EXIT_SUCCESS;
        }
        o = json_node_get_object(reply);
        if (g_strcmp0(verb, "invites") == 0) {
            list = json_object_get_array_member(o, "invites");
            if (list == NULL || json_array_get_length(list) == 0)
                g_print("No open invites.\n");
            for (i = 0; list != NULL && i < json_array_get_length(list); i++) {
                JsonObject *inv = json_array_get_object_element(list, i);
                g_print("%-14s %-10s %s\n", member_or(inv, "id", "?"),
                        member_or(inv, "tier", "?"), member_or(inv, "note", ""));
            }
            return EXIT_SUCCESS;
        }
        list = json_object_get_array_member(o, "peers");
        if (list == NULL || json_array_get_length(list) == 0) {
            g_print("No linked fleets.  `clawtilla federation invite` makes an invite;\n"
                    "`clawtilla federation link` links a daemon you own.\n");
            return EXIT_SUCCESS;
        }
        for (i = 0; i < json_array_get_length(list); i++)
            fed_print_peer_line(json_array_get_object_element(list, i));
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "uninvite") == 0) {
        if (alias == NULL) {
            g_printerr("Usage: clawtilla federation uninvite ID\n");
            return EXIT_FAILURE;
        }
        reply = fed_call(client, "federation.invite.revoke", build_payload("id", alias, NULL), 0);
        if (reply == NULL)
            return EXIT_FAILURE;
        g_print("Invite %s withdrawn.\n", alias);
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "log") == 0) {
        JsonArray *rows;
        g_autoptr(JsonBuilder) b = json_builder_new();
        const gchar *limit = fed_option(argc, argv, "--limit");

        json_builder_begin_object(b);
        if (alias != NULL) {
            json_builder_set_member_name(b, "alias");
            json_builder_add_string_value(b, alias);
        }
        json_builder_set_member_name(b, "limit");
        json_builder_add_int_value(b, limit != NULL ? g_ascii_strtoll(limit, NULL, 10) : 50);
        json_builder_end_object(b);
        reply = fed_call(client, "federation.audit", json_builder_get_root(b), 0);
        if (reply == NULL)
            return EXIT_FAILURE;
        if (json) {
            fed_print_json(reply);
            return EXIT_SUCCESS;
        }
        rows = json_object_get_array_member(json_node_get_object(reply), "entries");
        for (i = 0; rows != NULL && i < json_array_get_length(rows); i++) {
            JsonObject *e = json_array_get_object_element(rows, i);
            g_autoptr(GDateTime) when = g_date_time_new_from_unix_local(
                json_object_get_int_member_with_default(e, "time", 0));
            g_autofree gchar *stamp = when != NULL ? g_date_time_format(when, "%F %T") : NULL;
            g_print("%s  %-5s %-16s %-14s %-8s %s\n", stamp != NULL ? stamp : "?",
                    member_or(e, "direction", "?"), member_or(e, "peer", ""),
                    member_or(e, "kind", ""), member_or(e, "outcome", ""),
                    member_or(e, "detail", ""));
        }
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "transcript") == 0) {
        JsonArray *rooms;
        const gchar *limit = fed_option(argc, argv, "--limit");
        g_autoptr(JsonBuilder) b = json_builder_new();

        if (alias == NULL) {
            g_printerr("Usage: clawtilla federation transcript ALIAS (--task ID | --agent AGENT)"
                       " [--limit N]\n");
            return EXIT_FAILURE;
        }
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "alias");
        json_builder_add_string_value(b, alias);
        if (fed_option(argc, argv, "--task") != NULL) {
            json_builder_set_member_name(b, "task");
            json_builder_add_string_value(b, fed_option(argc, argv, "--task"));
        }
        if (fed_option(argc, argv, "--agent") != NULL) {
            json_builder_set_member_name(b, "agent");
            json_builder_add_string_value(b, fed_option(argc, argv, "--agent"));
        }
        if (limit != NULL) {
            json_builder_set_member_name(b, "limit");
            json_builder_add_int_value(b, g_ascii_strtoll(limit, NULL, 10));
        }
        json_builder_end_object(b);
        reply = fed_call(client, "federation.transcript", json_builder_get_root(b), 30);
        if (reply == NULL)
            return EXIT_FAILURE;
        o = json_node_get_object(reply);
        if (json) {
            g_autofree gchar *text = json_to_string(reply, TRUE);
            g_print("%s\n", text);
            return EXIT_SUCCESS;
        }
        if (json_object_has_member(o, "task")) {
            JsonObject *t = json_object_get_object_member(o, "task");
            g_print("Task there: %s%s%s\n", member_or(t, "state", "?"),
                    json_object_has_member(t, "progress") ? " -- " : "",
                    member_or(t, "progress", ""));
            if (json_object_has_member(t, "result"))
                g_print("Result: %s\n", member_or(t, "result", ""));
        }
        rooms = json_object_get_array_member(o, "rooms");
        for (i = 0; rooms != NULL && i < json_array_get_length(rooms); i++) {
            JsonObject *r = json_array_get_object_element(rooms, i);
            JsonArray *messages = json_object_get_array_member(r, "messages");
            guint j;

            g_print("\n== %s\n", member_or(r, "title", member_or(r, "room", "?")));
            for (j = 0; messages != NULL && j < json_array_get_length(messages); j++) {
                JsonObject *m = json_array_get_object_element(messages, j);
                g_print("%-16s %s\n", member_or(m, "sender", "?"), member_or(m, "body", ""));
            }
        }
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "rotate-key") == 0) {
        reply = fed_call(client, "federation.rotate_key", NULL, 0);
        if (reply == NULL)
            return EXIT_FAILURE;
        o = json_node_get_object(reply);
        g_print("New key in use (version %" G_GINT64_FORMAT "); %" G_GINT64_FORMAT
                " linked fleet(s) told.  The fleet id stays %s.\n",
                json_object_get_int_member_with_default(o, "key_index", 0),
                json_object_get_int_member_with_default(o, "told", 0),
                member_or(o, "fleet_id", "?"));
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "expose") == 0) {
        reply = fed_call(client, "federation.expose",
                         build_payload("hostname", fed_option(argc, argv, "--hostname"), NULL), 0);
        if (reply == NULL)
            return EXIT_FAILURE;
        o = json_node_get_object(reply);
        g_print("%s\n", member_or(o, "cloudflared_config", ""));
        g_print("# then: clawtilla config set federation.public_url %s\n",
                member_or(o, "public_url", ""));
        g_print("# steps: %s\n", member_or(o, "next", ""));
        return EXIT_SUCCESS;
    }

    /* Everything below names one linked fleet. */
    if (alias == NULL) {
        g_printerr("Usage: clawtilla federation %s ALIAS  (clawtilla federation list)\n", verb);
        return EXIT_FAILURE;
    }

    if (g_strcmp0(verb, "show") == 0 || g_strcmp0(verb, "explain") == 0) {
        reply = fed_call(client, "federation.peer.show", build_payload("alias", alias, NULL), 0);
        if (reply == NULL)
            return EXIT_FAILURE;
        if (json)
            fed_print_json(reply);
        else
            fed_print_show(json_node_get_object(reply));
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "set") == 0) {
        g_autoptr(JsonBuilder) b = json_builder_new();
        gint k;

        json_builder_begin_object(b);
        json_builder_set_member_name(b, "alias");
        json_builder_add_string_value(b, alias);
#define OPT(flag, key) do { const gchar *v_ = fed_option(argc, argv, flag); \
            if (v_ != NULL) { json_builder_set_member_name(b, key); \
                              json_builder_add_string_value(b, v_); } } while (0)
        OPT("--tier", "tier");
        OPT("--label", "label");
        OPT("--owner", "owner");
        OPT("--endpoint", "endpoint");
        OPT("--on-revoke", "on_revoke");
#undef OPT
        json_builder_set_member_name(b, "grant");
        json_builder_begin_object(b);
        for (k = 4; k < argc; k++) {
            const gchar *eq = strchr(argv[k], '=');
            g_autofree gchar *field = NULL;
            if (g_str_has_prefix(argv[k], "--") || eq == NULL)
                continue;
            field = g_strndup(argv[k], eq - argv[k]);
            json_builder_set_member_name(b, field);
            json_builder_add_string_value(b, eq + 1);
        }
        json_builder_end_object(b);
        json_builder_end_object(b);
        reply = fed_call(client, "federation.peer.set", json_builder_get_root(b), 0);
        if (reply == NULL)
            return EXIT_FAILURE;
        fed_print_show(json_node_get_object(reply));
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "tag") == 0) {
        g_autoptr(JsonBuilder) b = json_builder_new();
        gint k;

        json_builder_begin_object(b);
        json_builder_set_member_name(b, "alias");
        json_builder_add_string_value(b, alias);
        json_builder_set_member_name(b, "add");
        json_builder_begin_array(b);
        for (k = 4; k < argc; k++)
            if (argv[k][0] == '+' || (argv[k][0] != '-' && argv[k][0] != '\0'))
                json_builder_add_string_value(b, argv[k][0] == '+' ? argv[k] + 1 : argv[k]);
        json_builder_end_array(b);
        json_builder_set_member_name(b, "remove");
        json_builder_begin_array(b);
        for (k = 4; k < argc; k++)
            if (argv[k][0] == '-' && argv[k][1] != '-')
                json_builder_add_string_value(b, argv[k] + 1);
        json_builder_end_array(b);
        json_builder_end_object(b);
        reply = fed_call(client, "federation.peer.tag", json_builder_get_root(b), 0);
        if (reply == NULL)
            return EXIT_FAILURE;
        fed_print_peer_line(json_node_get_object(reply));
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "hide") == 0 || g_strcmp0(verb, "unhide") == 0 ||
        g_strcmp0(verb, "mute") == 0 || g_strcmp0(verb, "unmute") == 0) {
        g_autoptr(JsonBuilder) b = json_builder_new();
        gboolean on = g_strcmp0(verb, "hide") == 0 || g_strcmp0(verb, "mute") == 0;

        json_builder_begin_object(b);
        json_builder_set_member_name(b, "alias");
        json_builder_add_string_value(b, alias);
        json_builder_set_member_name(b, "value");
        json_builder_add_boolean_value(b, on);
        json_builder_end_object(b);
        reply = fed_call(client, g_str_has_suffix(verb, "hide") ? "federation.peer.hide"
                                                                : "federation.peer.mute",
                         json_builder_get_root(b), 0);
        if (reply == NULL)
            return EXIT_FAILURE;
        fed_print_peer_line(json_node_get_object(reply));
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "approve") == 0 || g_strcmp0(verb, "revoke") == 0 ||
        g_strcmp0(verb, "forget") == 0) {
        const gchar *kind = g_strcmp0(verb, "approve") == 0 ? "federation.peer.approve"
                          : g_strcmp0(verb, "revoke") == 0 ? "federation.peer.revoke"
                                                           : "federation.peer.forget";
        reply = fed_call(client, kind, build_payload("alias", alias, NULL), 0);
        if (reply == NULL)
            return EXIT_FAILURE;
        g_print("%s: %s.\n", alias, g_strcmp0(verb, "approve") == 0 ? "approved"
                                   : g_strcmp0(verb, "revoke") == 0 ? "revoked"
                                                                    : "forgotten");
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "ping") == 0) {
        reply = fed_call(client, "federation.peer.ping", build_payload("alias", alias, NULL),
                         FED_WAIT_SECONDS);
        if (reply == NULL)
            return EXIT_FAILURE;
        g_print("%s answered in %" G_GINT64_FORMAT " ms.\n", alias,
                json_object_get_int_member_with_default(json_node_get_object(reply), "ms", 0));
        return EXIT_SUCCESS;
    }

    g_printerr("clawtilla: unknown federation verb '%s' (clawtilla federation --help)\n", verb);
    return EXIT_FAILURE;
}


static gint
cmd_office(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    const gchar *verb = argc > 2 ? argv[2] : NULL;
    gboolean json = argc > 3 && g_strcmp0(argv[argc - 1], "--json") == 0;
    JsonObject *root;
    guint i;

    if (verb == NULL || g_strcmp0(verb, "-h") == 0 || g_strcmp0(verb, "--help") == 0) {
        g_print("Usage: clawtilla office <who|worlds|layout|choreography|on|off|use|renderer|\n"
                "                        custodians|character|open>\n"
                "\n"
                "The office draws the fleet as people in a building: at a desk\n"
                "while a turn runs, on the phone while two agents message, round a\n"
                "table in a group round.  It is on when office.enabled is true.\n"
                "\n"
                "  who           who is where, and since when\n"
                "  worlds        the world packs that can be drawn, and any refused\n"
                "  layout        which seat the director gives each participant\n"
                "  choreography  the built-in rules: zone, pose, prop per activity\n"
                "  on, off       turn the office on or off, at once\n"
                "  use WORLD     draw this world pack\n"
                "  renderer R    auto, 2d or 3d\n"
                "  custodians    who keeps the office, and the asset generator\n"
                "  custodians install [--team NAME]\n"
                "                make the custodial team: a lead, an asset steward\n"
                "                and a set designer, tagged office-custodial\n"
                "  character AGENT [--direct]\n"
                "                ask the custodians for AGENT's character again; with\n"
                "                --direct the daemon makes it through office.asset_api,\n"
                "                after you approve the estimate\n"
                "  api-key SERVICE\n"
                "                store the key office.asset_api uses (read from stdin)\n"
                "  peers         the other fleets visiting (office.peers), and whether\n"
                "                each answers\n"
                "  peer-pair NAME CODE\n"
                "                pair with a tailnet peer, using a code `clawtilla device\n"
                "                pair` printed on that machine\n"
                "  open          start clawtilla-office (built with OFFICE=1)\n"
                "\n"
                "Examples:\n"
                "  clawtilla office on\n"
                "  clawtilla office use cozy-town\n"
                "  clawtilla office who\n"
                "  clawtilla office worlds --json\n"
                "  clawtilla office choreography > my-rules.yaml\n"
                "  clawtilla office custodians install --team groundskeepers\n"
                "  clawtilla office character wren\n"
                "  clawtilla office api-key higgsfield\n"
                "  clawtilla office character wren --direct\n"
                "  clawtilla office peers\n"
                "  clawtilla office peer-pair studio K7Q-4MZ\n");
        return verb == NULL ? EXIT_FAILURE : EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "choreography") == 0) {
        g_print("%s", clawt_office_choreography_default_yaml());
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "open") == 0) {
        g_autofree gchar *exe = g_file_read_link("/proc/self/exe", NULL);
        g_autofree gchar *dir = exe != NULL ? g_path_get_dirname(exe) : NULL;
        g_autofree gchar *beside = dir != NULL
            ? g_build_filename(dir, "clawtilla-office", NULL) : NULL;
        const gchar *program = beside != NULL && g_file_test(beside, G_FILE_TEST_IS_EXECUTABLE)
            ? beside : "clawtilla-office";
        g_autoptr(GPtrArray) args = g_ptr_array_new();
        g_autoptr(GError) error = NULL;
        gint k;

        g_ptr_array_add(args, (gpointer)program);
        for (k = 3; k < argc; k++)
            g_ptr_array_add(args, argv[k]);
        g_ptr_array_add(args, NULL);
        execvp(program, (gchar **)args->pdata);
        g_printerr("clawtilla: could not start %s: %s.  It is built with "
                   "`make OFFICE=1 LRG_BACKEND=1`.\n", program, g_strerror(errno));
        return EXIT_FAILURE;
    }

    /* on, off, which world, how to draw it: office.set, applied at once */
    if (g_strcmp0(verb, "on") == 0 || g_strcmp0(verb, "off") == 0 ||
        g_strcmp0(verb, "use") == 0 || g_strcmp0(verb, "renderer") == 0) {
        g_autoptr(JsonBuilder) builder = json_builder_new();
        JsonObject *answer;

        if ((g_strcmp0(verb, "use") == 0 || g_strcmp0(verb, "renderer") == 0) && argc < 4) {
            g_printerr("Usage: clawtilla office %s <%s>\n", verb,
                       g_strcmp0(verb, "use") == 0 ? "world" : "auto|2d|3d");
            return EXIT_FAILURE;
        }
        json_builder_begin_object(builder);
        if (g_strcmp0(verb, "use") == 0) {
            json_builder_set_member_name(builder, "world");
            json_builder_add_string_value(builder, argv[3]);
        } else if (g_strcmp0(verb, "renderer") == 0) {
            json_builder_set_member_name(builder, "renderer");
            json_builder_add_string_value(builder, argv[3]);
        } else {
            json_builder_set_member_name(builder, "enabled");
            json_builder_add_boolean_value(builder, g_strcmp0(verb, "on") == 0);
        }
        json_builder_end_object(builder);

        client = connect_to_daemon();
        if (client == NULL)
            return EXIT_FAILURE;
        reply = call(client, "office.set", json_builder_get_root(builder));
        if (reply == NULL)
            return EXIT_FAILURE;
        answer = json_node_get_object(reply);
        g_print("The office is %s; world %s, drawn %s.\n",
                json_object_get_boolean_member_with_default(answer, "enabled", FALSE)
                    ? "on" : "off",
                member_or(answer, "world", "?"), member_or(answer, "renderer", "auto"));
        return EXIT_SUCCESS;
    }

    /* the key the daemon makes characters with: stdin, echo off, never argv */
    /* other fleets' people, visiting: which, and whether each answers */
    if (g_strcmp0(verb, "peers") == 0) {
        JsonArray *peers;
        guint k;

        client = connect_to_daemon();
        if (client == NULL)
            return EXIT_FAILURE;
        reply = call(client, "office.peers", NULL);
        if (reply == NULL)
            return EXIT_FAILURE;
        if (json) {
            g_autofree gchar *text = json_to_string(reply, TRUE);

            g_print("%s\n", text);
            return EXIT_SUCCESS;
        }
        peers = json_object_get_array_member(json_node_get_object(reply), "peers");
        if (peers == NULL || json_array_get_length(peers) == 0) {
            g_print("No peer fleets (office.peers is empty).\n");
            return EXIT_SUCCESS;
        }
        for (k = 0; k < json_array_get_length(peers); k++) {
            JsonObject *peer = json_array_get_object_element(peers, k);
            const gchar *state = member_or(peer, "state", "?");

            g_print("%-12s %-8s %3" G_GINT64_FORMAT " visiting  %s%s%s\n",
                    member_or(peer, "name", "?"), state,
                    json_object_get_int_member_with_default(peer, "visitors", 0),
                    member_or(peer, "where", "?"),
                    json_object_has_member(peer, "error") ? "  -- " : "",
                    member_or(peer, "error", ""));
        }
        return EXIT_SUCCESS;
    }

    /* a one-time code, so an argument is acceptable where a password is not */
    if (g_strcmp0(verb, "peer-pair") == 0) {
        if (argc < 5) {
            g_printerr("Usage: clawtilla office peer-pair NAME CODE\n");
            return EXIT_FAILURE;
        }
        client = connect_to_daemon();
        if (client == NULL)
            return EXIT_FAILURE;
        reply = call(client, "office.peer_pair",
                     build_payload("name", argv[3], "code", argv[4], NULL));
        if (reply == NULL)
            return EXIT_FAILURE;
        g_print("Paired with '%s'; its token is in %s.\n", argv[3],
                member_or(json_node_get_object(reply), "token_file", "?"));
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "api-key") == 0) {
        g_autofree gchar *key = NULL;
        JsonObject *answer;

        /*
         * The service is checked before the prompt: `--help` or a typo
         * used to be taken as the service, so the person was asked for
         * a secret, typed it, and only then heard the name was wrong.
         */
        if (argc >= 4 && (g_strcmp0(argv[3], "-h") == 0 || g_strcmp0(argv[3], "--help") == 0)) {
            g_print("Usage: clawtilla office api-key <higgsfield|comfyui>\n"
                    "Reads the key from the terminal with echo off and stores it for the daemon.\n"
                    "  higgsfield  KEY_ID:KEY_SECRET from console.higgsfield.ai\n"
                    "  comfyui     a bearer token, for a ComfyUI behind an auth proxy\n");
            return EXIT_SUCCESS;
        }
        if (argc < 4 || (g_strcmp0(argv[3], "higgsfield") != 0 &&
                         g_strcmp0(argv[3], "comfyui") != 0)) {
            g_printerr("Usage: clawtilla office api-key <higgsfield|comfyui>\n");
            return EXIT_FAILURE;
        }
        key = read_secret(g_strcmp0(argv[3], "higgsfield") == 0
                          ? "Key (KEY_ID:KEY_SECRET): " : "Token: ");
        if (key == NULL || *key == '\0') {
            g_printerr("clawtilla: no key was given\n");
            return EXIT_FAILURE;
        }
        client = connect_to_daemon();
        if (client == NULL)
            return EXIT_FAILURE;
        reply = call(client, "office.api_key", build_payload("service", argv[3], "key", key,
                                                             NULL));
        if (reply == NULL)
            return EXIT_FAILURE;
        answer = json_node_get_object(reply);
        g_print("Stored in %s.\n", member_or(answer, "key_file", "?"));
        return EXIT_SUCCESS;
    }

    /* the custodial team, and a character asked for again */
    if (g_strcmp0(verb, "custodians") == 0 || g_strcmp0(verb, "character") == 0) {
        g_autoptr(JsonBuilder) builder = json_builder_new();
        gboolean install = g_strcmp0(verb, "custodians") == 0 && argc > 3 &&
                           g_strcmp0(argv[3], "install") == 0;
        gboolean character = g_strcmp0(verb, "character") == 0;
        JsonObject *answer;
        gint k;

        if (character && (argc < 4 || argv[3][0] == '-')) {
            g_printerr("Usage: clawtilla office character <agent>\n");
            return EXIT_FAILURE;
        }
        json_builder_begin_object(builder);
        if (character) {
            json_builder_set_member_name(builder, "agent");
            json_builder_add_string_value(builder, argv[3]);
            for (k = 4; k < argc; k++)
                if (g_strcmp0(argv[k], "--direct") == 0) {
                    json_builder_set_member_name(builder, "direct");
                    json_builder_add_boolean_value(builder, TRUE);
                }
        }
        for (k = 4; install && k + 1 < argc; k++)
            if (g_strcmp0(argv[k], "--team") == 0) {
                json_builder_set_member_name(builder, "team");
                json_builder_add_string_value(builder, argv[++k]);
            }
        json_builder_end_object(builder);

        client = connect_to_daemon();
        if (client == NULL)
            return EXIT_FAILURE;
        reply = call(client, character ? "office.character"
                             : install ? "office.custodians_install" : "office.custodians",
                     json_builder_get_root(builder));
        if (reply == NULL)
            return EXIT_FAILURE;
        answer = json_node_get_object(reply);
        if (json) {
            g_autofree gchar *text = json_to_string(reply, TRUE);

            g_print("%s\n", text);
        } else if (character && json_object_has_member(answer, "asked") &&
                   g_strcmp0(member_or(answer, "service", ""), "comfyui") == 0) {
            g_print("Making %s's character on your ComfyUI: it costs nothing, so no card. "
                    "It is drawn in the office when it is done; the daemon log says why if "
                    "it is not.\n", argv[3]);
        } else if (character && json_object_has_member(answer, "asked")) {
            g_print("Asked: the estimate for %s's character will be in your decisions; "
                    "nothing is spent until you approve it.\n", argv[3]);
        } else if (character) {
            g_print("Asked for %s's character: task %s.\n", argv[3],
                    member_or(answer, "task", "?"));
        } else if (install) {
            JsonArray *made = json_object_get_array_member(answer, "agents");

            g_print("Installed the custodial team %s:", member_or(answer, "team", "?"));
            for (i = 0; made != NULL && i < json_array_get_length(made); i++)
                g_print(" %s", json_array_get_string_element(made, i));
            g_print(".\n");
        } else {
            const gchar *generator = member_or(answer, "generator", "");

            g_print("Custodial team: %s\nAsset generator: %s\n",
                    member_or(answer, "team", "none (clawtilla office custodians install)"),
                    *generator != '\0' ? generator : "none (office.asset_generator)");
        }
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "who") != 0 && g_strcmp0(verb, "worlds") != 0 &&
        g_strcmp0(verb, "layout") != 0) {
        g_printerr("clawtilla office: unknown verb '%s'\n", verb);
        return EXIT_FAILURE;
    }

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;
    reply = call(client, g_strcmp0(verb, "who") == 0 ? "office.snapshot"
                         : g_strcmp0(verb, "worlds") == 0 ? "office.worlds" : "office.layout",
                 NULL);
    if (reply == NULL)
        return EXIT_FAILURE;
    if (json) {
        g_autofree gchar *text = json_to_string(reply, TRUE);

        g_print("%s\n", text);
        return EXIT_SUCCESS;
    }
    root = json_node_get_object(reply);

    if (g_strcmp0(verb, "worlds") == 0) {
        JsonArray *worlds = json_object_get_array_member(root, "worlds");
        JsonArray *refused = json_object_get_array_member(root, "refused");

        for (i = 0; worlds != NULL && i < json_array_get_length(worlds); i++) {
            JsonObject *w = json_array_get_object_element(worlds, i);
            const gchar *id = member_or(w, "id", "?");

            g_print("%s %-16s %-24s %s%s%s\n",
                    g_strcmp0(id, member_or(root, "current", NULL)) == 0 ? "*" : " ",
                    id, member_or(w, "name", ""), member_or(w, "license", "?"),
                    json_object_get_boolean_member_with_default(w, "has_2d", FALSE) ? " 2d" : "",
                    json_object_get_boolean_member_with_default(w, "has_3d", FALSE) ? " 3d" : "");
        }
        for (i = 0; refused != NULL && i < json_array_get_length(refused); i++)
            g_printerr("clawtilla: refused %s\n", json_array_get_string_element(refused, i));
        return EXIT_SUCCESS;
    }

    if (!json_object_get_boolean_member_with_default(root, "enabled", TRUE)) {
        g_print("%s\n", member_or(root, "why", "the office is off"));
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "who") == 0) {
        JsonArray *people = json_object_get_array_member(root, "participants");
        gint64 now = json_object_get_int_member_with_default(root, "now",
                                                             g_get_real_time() / G_USEC_PER_SEC);

        if (people == NULL || json_array_get_length(people) == 0)
            g_print("(nobody in the office)\n");
        for (i = 0; people != NULL && i < json_array_get_length(people); i++) {
            JsonObject *p = json_array_get_object_element(people, i);
            g_autofree gchar *line = clawt_office_roster_line(p, now);

            g_print("%-20s %s\n", member_or(p, "id", "?"), line);
        }
        return EXIT_SUCCESS;
    }

    {
        JsonArray *placements = json_object_get_array_member(root, "placements");

        for (i = 0; placements != NULL && i < json_array_get_length(placements); i++) {
            JsonObject *p = json_array_get_object_element(placements, i);

            g_print("%-20s %-14s %-14s seat %-3" G_GINT64_FORMAT " %s%s%s\n",
                    member_or(p, "id", "?"), member_or(p, "activity", "?"),
                    member_or(p, "zone", "-"),
                    json_object_get_int_member_with_default(p, "seat", 0),
                    member_or(p, "pose", ""),
                    json_object_has_member(p, "prop") ? " holding " : "",
                    member_or(p, "prop", ""));
        }
    }
    return EXIT_SUCCESS;
}

static gboolean
limits_pending(JsonObject *root)
{
    JsonArray *logins = json_object_get_array_member(root, "logins");
    guint i;

    for (i = 0; logins != NULL && i < json_array_get_length(logins); i++)
        if (g_strcmp0(member_or(json_array_get_object_element(logins, i),
                                "state", NULL), "pending") == 0)
            return TRUE;
    return FALSE;
}

/*
 * clawtilla limits -- what each provider login has left of its plan.
 *
 * The daemon reads a login by running its CLI, behind the reply, so this
 * asks for a refresh and then re-reads until nothing is pending -- a
 * person at a terminal wants the answer, not "asking the provider...".
 * Every line is the library's (clawt_usage_window_*), so the figures
 * here are the ones the Fleet page shows.
 */
static gint
cmd_limits(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    gboolean wait = TRUE;
    gboolean json = FALSE;
    JsonObject *root;
    JsonArray *logins;
    gint64 now;
    gint64 deadline;
    gint i;

    for (i = 2; i < argc; i++) {
        if (g_strcmp0(argv[i], "--cached") == 0) {
            wait = FALSE;
        } else if (g_strcmp0(argv[i], "--json") == 0) {
            json = TRUE;
        } else if (g_strcmp0(argv[i], "-h") == 0 || g_strcmp0(argv[i], "--help") == 0) {
            g_print("Usage: clawtilla limits [--cached] [--json]\n"
                    "\n"
                    "What each provider login has left of its plan: the\n"
                    "five-hour and weekly windows, credit balances, and\n"
                    "when each one resets.  A login is one CLI signed in\n"
                    "one way -- the default login of each CLI an agent\n"
                    "uses, plus every entry under accounts: -- so agents\n"
                    "sharing a login share one row.\n"
                    "\n"
                    "Each login is read by running its CLI, which takes a\n"
                    "few seconds; this waits for the answers (up to 60s)\n"
                    "unless --cached, which prints what the daemon already\n"
                    "knows.  \"Reports no plan limits\" (an API key, a CLI\n"
                    "with no reporting interface) is an answer, not a\n"
                    "failure, and an unknown figure is never shown as 0.\n"
                    "\n"
                    "Examples:\n"
                    "  clawtilla limits            # read every login now\n"
                    "  clawtilla limits --cached   # what is known, instantly\n"
                    "  clawtilla limits --json     # the usage.providers reply\n");
            return EXIT_SUCCESS;
        } else {
            g_printerr("clawtilla limits: unknown option '%s'\n", argv[i]);
            return EXIT_FAILURE;
        }
    }

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    reply = call(client, "usage.providers",
                 build_payload("refresh", wait ? "true" : "false", NULL));
    if (reply == NULL)
        return EXIT_FAILURE;

    /* each CLI answers in its own time; re-read until none is pending */
    deadline = g_get_monotonic_time() + 60 * G_USEC_PER_SEC;
    while (wait && limits_pending(json_node_get_object(reply)) &&
           g_get_monotonic_time() < deadline) {
        g_usleep(G_USEC_PER_SEC / 2);
        g_clear_pointer(&reply, json_node_unref);
        reply = call(client, "usage.providers", NULL);
        if (reply == NULL)
            return EXIT_FAILURE;
    }

    if (json) {
        g_autofree gchar *text = json_to_string(reply, TRUE);

        g_print("%s\n", text);
        return EXIT_SUCCESS;
    }

    root = json_node_get_object(reply);
    now = json_object_get_int_member_with_default(root, "now",
                                                  g_get_real_time() / G_USEC_PER_SEC);
    logins = json_object_get_array_member(root, "logins");
    if (logins == NULL || json_array_get_length(logins) == 0) {
        g_print("(no logins: no agent runs a CLI provider)\n");
        return EXIT_SUCCESS;
    }

    for (i = 0; i < (gint)json_array_get_length(logins); i++) {
        JsonObject *login = json_array_get_object_element(logins, (guint)i);
        JsonArray *windows = json_object_get_array_member(login, "windows");
        g_autofree gchar *title = clawt_usage_login_title(login);
        g_autofree gchar *status = clawt_usage_login_status_label(login, now);
        guint w;

        g_print("%s%s\n  %s\n", i > 0 ? "\n" : "", title, status);
        for (w = 0; windows != NULL && w < json_array_get_length(windows); w++) {
            JsonObject *window = json_array_get_object_element(windows, w);
            g_autofree gchar *value = clawt_usage_window_value_label(window);
            g_autofree gchar *reset = clawt_usage_window_reset_label(window, now);
            g_autofree gchar *bar = NULL;
            gdouble left = 0.0;

            bar = clawt_usage_window_remaining(window, &left)
                ? clawt_usage_meter_text(left, 20)
                : g_strnfill(22, ' ');
            g_print("  %-18s %s  %-24s %s\n", member_or(window, "label", "?"),
                    bar, value, reset != NULL ? reset : "");
        }
    }
    return EXIT_SUCCESS;
}

/*
 * clawtilla cost --by / --csv -- the ledger, grouped.
 *
 * A separate frame from the per-agent summary because it answers a
 * different question: not "which agent", but "which model, which
 * routine, which day".  The CSV is the library's, so this file and the
 * clients' Export buttons write the same document.
 */
static gint
cost_ledger(
	gint64       since,
	const gchar *by,
	const gchar *only_agent,
	gboolean     csv
){
	g_autoptr(ClawtClient) client = NULL;
	g_autoptr(JsonNode) reply = NULL;
	JsonObject *root;
	JsonObject *total;
	JsonArray *groups;
	guint i;

	client = connect_to_daemon();
	if (client == NULL)
		return EXIT_FAILURE;

	reply = call(client, "usage.ledger",
		      clawt_usage_ledger_payload(since, by, only_agent));
	if (reply == NULL)
		return EXIT_FAILURE;

	root = json_node_get_object(reply);

	if (csv) {
		g_autofree gchar *document = clawt_usage_ledger_csv(root);

		fputs(document, stdout);
		return EXIT_SUCCESS;
	}

	groups = json_object_get_array_member(root, "groups");
	total = json_object_get_object_member(root, "total");

	{
		g_autofree gchar *heading = g_ascii_strup(
			member_or(root, "by", "agent"), -1);

		g_print("%-28s %8s %9s %12s %12s %12s\n", heading, "TURNS",
			"UNPRICED", "IN", "OUT", "COST");
	}

	for (i = 0; groups != NULL && i < json_array_get_length(groups); i++) {
		JsonObject *group = json_array_get_object_element(groups, i);
		g_autofree gchar *cost = clawt_usage_ledger_cost_label(group);

		g_print("%-28s %8" G_GINT64_FORMAT " %9" G_GINT64_FORMAT
			" %12" G_GINT64_FORMAT " %12" G_GINT64_FORMAT " %12s\n",
			member_or(group, "key", "?"),
			json_object_get_int_member_with_default(group, "turns", 0),
			json_object_get_int_member_with_default(group, "unpriced_turns", 0),
			json_object_get_int_member_with_default(group, "input_tokens", 0),
			json_object_get_int_member_with_default(group, "output_tokens", 0),
			cost);
	}

	if (groups == NULL || json_array_get_length(groups) == 0) {
		g_print("(nothing recorded)\n");
		return EXIT_SUCCESS;
	}

	{
		g_autofree gchar *cost = clawt_usage_ledger_cost_label(total);
		g_autofree gchar *unpriced = clawt_usage_ledger_unpriced_line(root);

		g_print("%-28s %8" G_GINT64_FORMAT " %9" G_GINT64_FORMAT
			" %12" G_GINT64_FORMAT " %12" G_GINT64_FORMAT " %12s\n",
			"total",
			json_object_get_int_member_with_default(total, "turns", 0),
			json_object_get_int_member_with_default(total, "unpriced_turns", 0),
			json_object_get_int_member_with_default(total, "input_tokens", 0),
			json_object_get_int_member_with_default(total, "output_tokens", 0),
			cost);

		/* said in words as well as in a column: it is the easy one to miss */
		if (unpriced != NULL)
			g_print("\n%s.\n", unpriced);
	}

	if (since > 0) {
		g_autoptr(GDateTime) from = g_date_time_new_from_unix_local(since);
		g_autofree gchar *stamp = g_date_time_format(from, "%Y-%m-%d %H:%M");

		g_print("\nSince %s.\n", stamp);
	}

	return EXIT_SUCCESS;
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
    const gchar *group_by = NULL;
    gboolean by_day = FALSE;
    gboolean csv = FALSE;
    gdouble budget;
    guint i;
    gint shown = 0;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;

    /*
     * Parsed here rather than through GOption because every other verb
     * in this file does the same: the subcommand owns its own words.
     */
    for (i = 2; i < (guint)argc; i++) {
        if (g_strcmp0(argv[i], "--today") == 0) {
            GDateTime *today = g_date_time_new_now_local();
            GDateTime *midnight = g_date_time_new_local(
                g_date_time_get_year(today), g_date_time_get_month(today),
                g_date_time_get_day_of_month(today), 0, 0, 0.0);

            since = g_date_time_to_unix(midnight);
            g_date_time_unref(midnight);
            g_date_time_unref(today);
        } else if (g_strcmp0(argv[i], "--days") == 0 && i + 1 < (guint)argc) {
            days = g_ascii_strtoll(argv[++i], NULL, 10);
            if (days > 0)
                since = (g_get_real_time() / G_USEC_PER_SEC) - days * 86400;
        } else if (g_strcmp0(argv[i], "--agent") == 0 && i + 1 < (guint)argc) {
            only_agent = argv[++i];
        } else if (g_strcmp0(argv[i], "--by-day") == 0) {
            by_day = TRUE;
        } else if (g_strcmp0(argv[i], "--by") == 0 && i + 1 < (guint)argc) {
            group_by = argv[++i];
            if (clawt_usage_group_index(group_by) < 0) {
                g_printerr("clawtilla cost: --by takes agent, model, origin "
                           "or day, not '%s'\n", group_by);
                return EXIT_FAILURE;
            }
        } else if (g_strcmp0(argv[i], "--csv") == 0) {
            csv = TRUE;
        } else if (g_strcmp0(argv[i], "-h") == 0 ||
                   g_strcmp0(argv[i], "--help") == 0) {
            g_print("Usage: clawtilla cost [--today | --days N] "
                    "[--agent ID] [--by-day]\n"
                    "       clawtilla cost --by agent|model|origin|day "
                    "[--csv] [--today | --days N] [--agent ID]\n"
                    "\n"
                    "What the fleet has spent, per agent and in total, most\n"
                    "first, with how long since each agent's last turn and\n"
                    "whether it is idle (orchestration.idle_hours).\n"
                    "--by-day adds the fleet's spend for each of the last\n"
                    "14 days (or --days N).\n"
                    "\n"
                    "--by groups the ledger instead: by agent, by model, by\n"
                    "origin (operator, agent:<id>, routine:<id>,\n"
                    "trigger:<id>, task:<id>, channel:<name>, untagged) or\n"
                    "by day.  UNPRICED counts turns with no recorded price;\n"
                    "they are not in COST, because unknown is not free.\n"
                    "--csv prints the same table as CSV (by agent unless\n"
                    "--by says otherwise), ready for a spreadsheet.\n"
                    "\n"
                    "Examples:\n"
                    "  clawtilla cost                # everything on record\n"
                    "  clawtilla cost --today        # since local midnight\n"
                    "  clawtilla cost --days 7       # the last week\n"
                    "  clawtilla cost --agent chief  # one agent\n"
                    "  clawtilla cost --by-day       # and the last 14 days\n"
                    "  clawtilla cost --by origin    # which routine costs most\n"
                    "  clawtilla cost --by model --days 30\n"
                    "  clawtilla cost --by day --csv > spending.csv\n");
            return EXIT_SUCCESS;
        } else {
            g_printerr("clawtilla cost: unknown option '%s'\n", argv[i]);
            return EXIT_FAILURE;
        }
    }

    if (group_by != NULL || csv) {
        if (by_day) {
            g_printerr("clawtilla cost: --by-day is the summary's strip; "
                       "use --by day with --by or --csv\n");
            return EXIT_FAILURE;
        }
        return cost_ledger(since, group_by != NULL ? group_by : "agent",
                           only_agent, csv);
    }

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "since");
    json_builder_add_int_value(builder, since);
    json_builder_set_member_name(builder, "sort");
    json_builder_add_string_value(builder, "cost");
    if (by_day) {
        json_builder_set_member_name(builder, "days");
        json_builder_add_int_value(builder, days > 0 ? days : 14);
    }
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

    now = json_object_get_int_member_with_default(root, "now", now);
    g_print("%-20s %8s %12s %12s %12s  %-10s %s\n",
            "AGENT", "TURNS", "IN", "OUT", "COST", "LAST TURN", "IDLE");

    for (i = 0; i < json_array_get_length(agents); i++) {
        JsonObject *a = json_array_get_object_element(agents, i);
        const gchar *id = member_or(a, "id", "?");
        g_autofree gchar *cost = NULL;

        if (only_agent != NULL && g_strcmp0(id, only_agent) != 0)
            continue;

        cost = clawt_usage_format_cost(
            json_object_get_int_member_with_default(a, "cost_micros", 0));

        {
            /* a daemon before 0.8.0 sends neither: say so, not "never" */
            g_autofree gchar *last = json_object_has_member(a, "last_turn_at")
                ? clawt_usage_last_turn_label(
                      json_object_get_int_member(a, "last_turn_at"), now)
                : g_strdup("-");

            g_print("%-20s %8" G_GINT64_FORMAT " %12" G_GINT64_FORMAT
                    " %12" G_GINT64_FORMAT " %12s  %-10s %s\n",
                    id,
                    json_object_get_int_member_with_default(a, "turns", 0),
                    json_object_get_int_member_with_default(a, "input_tokens", 0),
                    json_object_get_int_member_with_default(a, "output_tokens", 0),
                    cost, last, member_or(a, "idle", "-"));
        }
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

    /* the fleet's days, oldest first, as the Fleet page draws them */
    if (by_day && json_object_has_member(root, "days")) {
        JsonArray *day_list = json_object_get_array_member(root, "days");
        g_autofree gchar *strip = clawt_usage_fleet_strip(root);
        guint d;

        g_print("\n%-12s %12s\n", "DAY", "COST");
        for (d = 0; d < json_array_get_length(day_list); d++) {
            JsonObject *day = json_array_get_object_element(day_list, d);
            g_autoptr(GDateTime) start = g_date_time_new_from_unix_local(
                json_object_get_int_member_with_default(day, "start", 0));
            g_autofree gchar *date = g_date_time_format(start, "%Y-%m-%d");
            g_autofree gchar *cost = clawt_usage_format_cost(
                json_object_get_int_member_with_default(day, "cost_micros", 0));

            g_print("%-12s %12s\n", date, cost);
        }
        g_print("%s\n", strip);
    }

    if (since > 0) {
        g_autoptr(GDateTime) from = g_date_time_new_from_unix_local(since);
        g_autofree gchar *stamp = g_date_time_format(from, "%Y-%m-%d %H:%M");

        g_print("\nSince %s.\n", stamp);
    }

    if (budget > 0.0)
        g_print("Each delegated task, with everything it hands on, may "
                "spend $%.2f (orchestration.task_budget_usd).\n", budget);

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
/*
 * What agents are waiting for: conditions the daemon checks by code on
 * their behalf.  `list` takes an optional agent; `cancel` is the
 * operator's, and the daemon tells the agent it is no longer waiting.
 */
static int
cmd_wait(
	int   argc,
	char *argv[]
){
	g_autoptr(ClawtClient) client = NULL;
	g_autoptr(JsonNode) reply = NULL;
	const gchar *verb = (argc > 2) ? argv[2] : "list";

	client = connect_to_daemon();
	if (client == NULL)
		return EXIT_FAILURE;

	if (g_strcmp0(verb, "list") == 0) {
		JsonArray *waits;
		gint64 now = g_get_real_time();
		guint i;

		reply = call(client, "wait.list",
		             (argc > 3) ? build_payload("agent", argv[3], NULL)
		                        : NULL);
		if (reply == NULL)
			return EXIT_FAILURE;

		waits = json_object_get_array_member(json_node_get_object(reply),
		                                     "waits");

		if (json_array_get_length(waits) == 0) {
			g_print("Nothing is being waited on.\n");
			return EXIT_SUCCESS;
		}

		for (i = 0; i < json_array_get_length(waits); i++) {
			JsonObject *wait = json_array_get_object_element(waits, i);
			g_autofree gchar *schedule = clawt_wait_schedule_label(
				json_object_get_int_member(wait, "last_check_at"),
				json_object_get_int_member(wait, "next_check_at"),
				json_object_get_int_member(wait, "expires_at"), now);

			g_print("%s  [%s]  %s\n", member_or(wait, "id", "?"),
			        member_or(wait, "agent", "?"),
			        member_or(wait, "label", ""));
			g_print("  %s\n  %s\n", member_or(wait, "condition", ""),
			        schedule);

			if (json_object_has_member(wait, "last_result"))
				g_print("  last check saw: %s\n",
				        member_or(wait, "last_result", ""));

			g_print("\n");
		}

		return EXIT_SUCCESS;
	}

	if (g_strcmp0(verb, "cancel") == 0) {
		if (argc < 4) {
			g_printerr("Usage: clawtilla wait cancel <id>\n"
			           "\n"
			           "The agent is told it is no longer waiting.\n");
			return EXIT_FAILURE;
		}

		reply = call(client, "wait.cancel",
		             build_payload("wait", argv[3], NULL));
		if (reply == NULL)
			return EXIT_FAILURE;

		g_print("Cancelled %s. The agent has been told.\n", argv[3]);
		return EXIT_SUCCESS;
	}

	g_printerr("Usage: clawtilla wait list [agent]\n"
	           "       clawtilla wait cancel <id>\n");
	return EXIT_FAILURE;
}

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

            /* Who it was put to first, when it came out of delegated work. */
            {
                g_autofree gchar *routed = clawt_decision_routed_label(
                    json_object_has_member(decision, "routed_to")
                        ? json_object_get_string_member(decision, "routed_to")
                        : NULL,
                    json_object_has_member(decision, "answered_by")
                        ? json_object_get_string_member(decision,
                                                        "answered_by")
                        : NULL,
                    json_object_get_int_member(decision, "state") ==
                        CLAWT_DECISION_OPEN);

                if (routed != NULL)
                    g_print("  %s\n", routed);
            }

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
                                   "answer", argv[4],
                                   "via", CLAWT_DECISION_VIA_CLI, NULL));
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

    if (g_strcmp0(verb, "delete") == 0) {
        if (argc < 4) {
            g_printerr("Usage: clawtilla decision delete <id>\n"
                       "\n"
                       "Removes it from the inbox for good, answered or not. "
                       "An open one is withdrawn first: the agent carries on "
                       "with its default.\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "decision.delete",
                     build_payload("decision", argv[3], NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("Deleted.\n");
        return EXIT_SUCCESS;
    }

    g_printerr("Usage: clawtilla decision [list [--all] | answer <id> "
               "<answer> | dismiss <id> | delete <id>]\n");
    return EXIT_FAILURE;
}

/*
 * clawtilla browser: the fleet's browser sessions.
 *
 * list reads the daemon's record and touches no browser; open, stop and
 * screenshot go to the agent's session, starting it for open.
 */
static gint
cmd_browser(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    const gchar *verb = argc > 2 ? argv[2] : "list";

    if (g_strcmp0(verb, "-h") == 0 || g_strcmp0(verb, "--help") == 0 ||
        g_strcmp0(verb, "help") == 0) {
        g_print("Usage: clawtilla browser [list | open AGENT URL | click AGENT X Y |\n"
                "                          stop AGENT | screenshot AGENT FILE |\n"
                "                          secrets AGENT]\n"
                "\n"
                "The web browsers agents drive (agents.browser.mode). A session\n"
                "starts on the agent's first browser call; open starts it too.\n"
                "\n"
                "Examples:\n"
                "  clawtilla browser                       # every session\n"
                "  clawtilla browser open research example.org\n"
                "  clawtilla browser screenshot research page.png\n"
                "  clawtilla browser click research 40 20   # page pixels\n"
                "  clawtilla browser stop research\n"
                "  clawtilla browser secrets research       # vault entries it may fill\n");
        return EXIT_SUCCESS;
    }

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    if (g_strcmp0(verb, "list") == 0) {
        JsonObject *root;
        JsonArray *sessions;
        guint i;

        reply = call(client, "browser.list", NULL);
        if (reply == NULL)
            return EXIT_FAILURE;
        root = json_node_get_object(reply);
        if (!json_object_get_boolean_member_with_default(root, "available", FALSE))
            g_print("This daemon was built without GSURF=1: no session can start.\n");
        sessions = json_object_has_member(root, "sessions")
            ? json_object_get_array_member(root, "sessions") : NULL;
        if (sessions == NULL || json_array_get_length(sessions) == 0) {
            g_print("No agent has a browser (agents.browser.mode).\n");
            return EXIT_SUCCESS;
        }
        for (i = 0; i < json_array_get_length(sessions); i++) {
            JsonObject *session = json_array_get_object_element(sessions, i);
            JsonArray *agents = json_object_get_array_member(session, "agents");
            g_autofree gchar *line = clawt_browser_session_line(session);
            g_autoptr(GString) who = g_string_new("");
            guint j;

            for (j = 0; agents != NULL && j < json_array_get_length(agents); j++)
                g_string_append_printf(who, "%s%s", j > 0 ? ", " : "",
                                       json_array_get_string_element(agents, j));
            g_print("%-20s %-7s %s\n  %s\n", member_or(session, "id", "?"),
                    member_or(session, "kind", "?"), who->len > 0 ? who->str : "(nobody)",
                    line);
        }
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "open") == 0 && argc > 4) {
        g_autofree gchar *line = NULL;

        reply = call(client, "browser.open",
                     clawt_ipc_payload_new("agent", argv[3], "url", argv[4], NULL));
        if (reply == NULL)
            return EXIT_FAILURE;
        line = clawt_browser_session_line(json_node_get_object(reply));
        g_print("%s\n", line);
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "click") == 0 && argc > 5) {
        g_autoptr(JsonBuilder) builder = json_builder_new();
        g_autofree gchar *line = NULL;

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "agent");
        json_builder_add_string_value(builder, argv[3]);
        json_builder_set_member_name(builder, "x");
        json_builder_add_int_value(builder, g_ascii_strtoll(argv[4], NULL, 10));
        json_builder_set_member_name(builder, "y");
        json_builder_add_int_value(builder, g_ascii_strtoll(argv[5], NULL, 10));
        json_builder_end_object(builder);
        reply = call(client, "browser.click", json_builder_get_root(builder));
        if (reply == NULL)
            return EXIT_FAILURE;
        line = clawt_browser_session_line(json_node_get_object(reply));
        g_print("%s\n", line);
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "stop") == 0 && argc > 3) {
        reply = call(client, "browser.stop", clawt_ipc_payload_new("agent", argv[3], NULL));
        if (reply == NULL)
            return EXIT_FAILURE;
        g_print("Stopped. The profile and its logins stay.\n");
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "screenshot") == 0 && argc > 4) {
        const gchar *encoded;
        g_autofree guchar *raw = NULL;
        g_autoptr(GError) error = NULL;
        gsize length = 0;

        reply = call(client, "browser.frame", clawt_ipc_payload_new("agent", argv[3], NULL));
        if (reply == NULL)
            return EXIT_FAILURE;
        encoded = member_or(json_node_get_object(reply), "png_base64", NULL);
        if (encoded == NULL &&
            json_object_get_boolean_member_with_default(json_node_get_object(reply),
                                                        "secret_hold", FALSE)) {
            g_printerr("A secret was filled on %s's page: no picture until it navigates "
                       "away.\n", argv[3]);
            return EXIT_FAILURE;
        }
        if (encoded == NULL) {
            g_printerr("%s's browser is not running; nothing to take a picture of.\n",
                       argv[3]);
            return EXIT_FAILURE;
        }
        raw = g_base64_decode(encoded, &length);
        if (!g_file_set_contents(argv[4], (const gchar *)raw, (gssize)length, &error)) {
            g_printerr("%s\n", error->message);
            return EXIT_FAILURE;
        }
        g_print("Wrote %s (%" G_GSIZE_FORMAT " bytes).\n", argv[4], length);
        return EXIT_SUCCESS;
    }

    /* The vault entries an agent may have filled: names, never values. */
    if (g_strcmp0(verb, "secrets") == 0 && argc > 3) {
        JsonObject *root;
        JsonArray *vault_entries;
        guint i;

        reply = call(client, "vault.list", clawt_ipc_payload_new("agent", argv[3], NULL));
        if (reply == NULL)
            return EXIT_FAILURE;
        root = json_node_get_object(reply);
        vault_entries = json_object_has_member(root, "entries")
            ? json_object_get_array_member(root, "entries") : NULL;
        if (vault_entries == NULL || json_array_get_length(vault_entries) == 0) {
            g_print("No vault entries (vault: in clawtilla.yaml).\n");
            return EXIT_SUCCESS;
        }
        for (i = 0; i < json_array_get_length(vault_entries); i++) {
            JsonObject *entry = json_array_get_object_element(vault_entries, i);
            g_autofree gchar *label = clawt_vault_entry_label(entry);

            g_print("[%s] %s\n",
                    json_object_get_boolean_member_with_default(entry, "granted", FALSE)
                        ? "x" : " ", label);
        }
        g_print("Change with: clawtilla agent set %s browser.secrets NAME,NAME\n", argv[3]);
        return EXIT_SUCCESS;
    }

    g_printerr("Unknown browser verb or missing argument; try clawtilla browser --help\n");
    return EXIT_FAILURE;
}

/*
 * Every turn running across the fleet, the listing the Activity page in
 * both clients draws. Stopping and steering are the existing verbs --
 * `agent interrupt` and `send` -- so they are named rather than
 * duplicated.
 */
static gint
cmd_activity(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *turns;
    gint64 now;
    guint i;

    (void)argc;
    (void)argv;

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;
    reply = call(client, "activity.list", NULL);
    if (reply == NULL)
        return EXIT_FAILURE;

    turns = json_object_has_member(json_node_get_object(reply), "turns")
        ? json_object_get_array_member(json_node_get_object(reply), "turns") : NULL;
    now = json_object_get_int_member_with_default(json_node_get_object(reply), "now",
                                                  g_get_real_time());
    if (turns == NULL || json_array_get_length(turns) == 0) {
        g_print("Nothing is running.\n");
        return EXIT_SUCCESS;
    }

    for (i = 0; i < json_array_get_length(turns); i++) {
        JsonObject *turn = json_array_get_object_element(turns, i);
        const gchar *agent_id = member_or(turn, "agent", "?");
        g_autofree gchar *elapsed = clawt_activity_elapsed_label(
            json_object_get_int_member_with_default(turn, "started", 0), now);
        JsonArray *steps = json_object_has_member(turn, "steps")
            ? json_object_get_array_member(turn, "steps") : NULL;

        g_print("%s  in %s  running %s%s%s%s%s\n", agent_id,
                member_or(turn, "room_name", member_or(turn, "room", "?")), elapsed,
                json_object_has_member(turn, "origin") ? "  for " : "",
                member_or(turn, "origin", ""),
                json_object_has_member(turn, "task") ? "  task " : "",
                member_or(turn, "task", ""));

        if (steps != NULL && json_array_get_length(steps) > 0) {
            g_autoptr(ClawtTurnStep) last = clawt_turn_step_new_from_object(
                json_array_get_object_element(steps, json_array_get_length(steps) - 1),
                agent_id);
            g_autofree gchar *summary = last != NULL ? clawt_turn_step_summary(last) : NULL;

            if (summary != NULL)
                g_print("  last: %s\n", summary);
        }
    }

    g_print("\nStop one with `clawtilla agent interrupt <agent>` (every turn it is "
            "running); steer one with `clawtilla send <agent> <text>`.\n");
    return EXIT_SUCCESS;
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
        g_autoptr(JsonBuilder) builder = json_builder_new();
        JsonArray *tasks;
        gboolean history = FALSE;
        gint arg;
        guint i;

        /*
         * --history reads every task the daemon still keeps, newest
         * first, a page at a time; the rest narrow it.  Without it the
         * list is the live one, as every client shows.
         */
        json_builder_begin_object(builder);

        for (arg = 3; arg < argc; arg++) {
            const gchar *opt = argv[arg];
            const gchar *value = (arg + 1 < argc) ? argv[arg + 1] : NULL;

            if (g_strcmp0(opt, "--history") == 0) {
                history = TRUE;
                json_builder_set_member_name(builder, "history");
                json_builder_add_boolean_value(builder, TRUE);
            } else if ((g_strcmp0(opt, "--state") == 0 ||
                        g_strcmp0(opt, "--agent") == 0) && value != NULL) {
                json_builder_set_member_name(builder, opt + 2);
                json_builder_add_string_value(builder, value);
                arg++;
            } else if ((g_strcmp0(opt, "--before") == 0 ||
                        g_strcmp0(opt, "--limit") == 0) && value != NULL) {
                json_builder_set_member_name(builder, opt + 2);
                json_builder_add_int_value(builder,
                                           g_ascii_strtoll(value, NULL, 10));
                arg++;
            } else {
                g_printerr("Usage: clawtilla task list [--history] "
                           "[--state STATE] [--agent AGENT] [--before UNIX] "
                           "[--limit N]\n");
                return EXIT_FAILURE;
            }
        }

        json_builder_end_object(builder);

        reply = call(client, "task.list", json_builder_get_root(builder));
        if (reply == NULL)
            return EXIT_FAILURE;

        tasks = json_object_get_array_member(json_node_get_object(reply),
                                             "tasks");

        /*
         * What the list covers, on stderr so a script reading the table
         * is unaffected: the open tasks and the last day's, or every
         * task when --history asked for them.
         */
        if (!history) {
            JsonObject *root = json_node_get_object(reply);
            g_autofree gchar *since = clawt_task_list_caption(
                json_object_get_int_member_with_default(root, "since", 0),
                json_object_get_boolean_member_with_default(root, "kept",
                                                            FALSE),
                json_object_get_int_member_with_default(root,
                                                        "retention_days", 0),
                g_get_real_time() / G_USEC_PER_SEC);

            g_printerr("%s\n", since);
        }

        if (json_array_get_length(tasks) == 0) {
            g_print("No tasks.\n");
            return EXIT_SUCCESS;
        }

        g_print("%-26s %-16s %-12s %-12s %-10s %s\n", "ID", "CREATED",
                "FROM", "TO", "STATE", "WORK");

        for (i = 0; i < json_array_get_length(tasks); i++) {
            JsonObject *task = json_array_get_object_element(tasks, i);
            gint64 created = json_object_get_int_member_with_default(
                task, "created_at", 0);
            g_autoptr(GDateTime) when = created > 0
                ? g_date_time_new_from_unix_local(created) : NULL;
            g_autofree gchar *stamp = when != NULL
                ? g_date_time_format(when, "%Y-%m-%d %H:%M") : g_strdup("-");

            /* The title rather than the whole prompt: one line a row. */
            g_print("%-26s %-16s %-12s %-12s %-10s %s\n",
                    member_or(task, "id", "?"), stamp,
                    member_or(task, "origin", "?"),
                    member_or(task, "assignee", "(backlog)"),
                    member_or(task, "state", "?"),
                    member_or(task, "title", member_or(task, "prompt", "")));
        }

        /*
         * The next page's cursor, when this one was full: the oldest
         * task's created_at, which --before takes.
         */
        if (history && json_array_get_length(tasks) > 0) {
            JsonObject *last = json_array_get_object_element(
                tasks, json_array_get_length(tasks) - 1);

            g_printerr("Older: clawtilla task list --history --before %"
                       G_GINT64_FORMAT "\n",
                       json_object_get_int_member_with_default(
                           last, "created_at", 0));
        }

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "show") == 0) {
        JsonObject *task;

        gboolean journal = FALSE;

        if (argc < 4) {
            g_printerr("Usage: clawtilla task show <task-id> [--journal]\n");
            return EXIT_FAILURE;
        }

        journal = (argc > 4 && g_strcmp0(argv[4], "--journal") == 0);

        {
            g_autoptr(JsonBuilder) builder = json_builder_new();

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "task");
            json_builder_add_string_value(builder, argv[3]);
            json_builder_set_member_name(builder, "journal");
            json_builder_add_boolean_value(builder, journal);
            json_builder_end_object(builder);

            reply = call(client, "task.show", json_builder_get_root(builder));
        }
        if (reply == NULL)
            return EXIT_FAILURE;

        task = json_object_get_object_member(json_node_get_object(reply),
                                             "task");

        g_print("id:       %s\n", member_or(task, "id", "?"));
        g_print("title:    %s\n", member_or(task, "title", ""));
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

        /* What it waits for, and which of that still holds it. */
        if (json_object_has_member(task, "after")) {
            JsonArray *after = json_object_get_array_member(task, "after");
            g_autofree gchar *waiting = clawt_task_waiting_caption(
                json_object_get_array_member(task, "waiting_on"));
            guint w;

            g_print("after:    ");
            for (w = 0; w < json_array_get_length(after); w++)
                g_print("%s%s", w > 0 ? ", " : "",
                        json_array_get_string_element(after, w));
            g_print("%s%s\n", *waiting != '\0' ? "  -- " : "", waiting);
        }

        /* The room it runs in, when it has one of its own. */
        if (json_object_has_member(task, "thread_room"))
            g_print("room:     %s  (clawtilla room history %s)\n",
                    json_object_get_string_member(task, "thread_room"),
                    json_object_get_string_member(task, "thread_room"));

        if (json_object_has_member(task, "consult"))
            g_print("mode:     %s -- the assignee could answer and do "
                    "nothing else\n", clawt_task_consult_label());

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

        /* Its VENTURE ticket, when the mirror has looked at it. */
        {
            JsonObject *root = json_node_get_object(reply);

            if (json_object_has_member(root, "venture")) {
                JsonObject *v = json_object_get_object_member(root, "venture");
                const gchar *vstate = member_or(v, "state", "?");

                if (json_object_has_member(v, "ticket"))
                    g_print("venture:  #%" G_GINT64_FORMAT " (%s)  %s\n",
                            json_object_get_int_member(v, "ticket"), vstate,
                            member_or(v, "url", ""));
                else
                    g_print("venture:  %s\n", vstate);

                if (json_object_has_member(v, "reason"))
                    g_print("          %s%s\n", member_or(v, "reason", ""),
                            (g_strcmp0(vstate, "dead") == 0 ||
                             g_strcmp0(vstate, "skipped") == 0)
                                ? "  (clawtilla task venture-retry)" : "");
            }
        }

        /* What was said about it, oldest first. */
        {
            JsonObject *root = json_node_get_object(reply);
            JsonArray *notes = json_object_has_member(root, "notes")
                ? json_object_get_array_member(root, "notes") : NULL;
            guint n;

            for (n = 0; notes != NULL && n < json_array_get_length(notes);
                 n++) {
                JsonObject *row = json_array_get_object_element(notes, n);
                g_autoptr(GDateTime) when = g_date_time_new_from_unix_local(
                    json_object_get_int_member_with_default(row, "at", 0));
                g_autofree gchar *stamp = when != NULL
                    ? g_date_time_format(when, "%Y-%m-%d %H:%M")
                    : g_strdup("?");

                if (n == 0)
                    g_print("\nthread:\n");

                g_print("  %s  %-8s %s: %s\n", stamp,
                        member_or(row, "kind", "?"),
                        member_or(row, "author", "clawtilla"),
                        member_or(row, "body", ""));
            }
        }

        /* What happened to it, oldest first. */
        if (journal) {
            JsonObject *root = json_node_get_object(reply);
            JsonArray *rows = json_object_has_member(root, "journal")
                ? json_object_get_array_member(root, "journal") : NULL;
            guint r;

            g_print("\nhistory:\n");

            if (rows == NULL || json_array_get_length(rows) == 0)
                g_print("  (none kept)\n");

            for (r = 0; rows != NULL && r < json_array_get_length(rows); r++) {
                JsonObject *row = json_array_get_object_element(rows, r);
                g_autoptr(GDateTime) when = g_date_time_new_from_unix_local(
                    json_object_get_int_member_with_default(row, "at", 0));
                g_autofree gchar *stamp = when != NULL
                    ? g_date_time_format(when, "%Y-%m-%d %H:%M:%S")
                    : g_strdup("?");

                g_print("  %s  %-10s %s\n", stamp,
                        member_or(row, "state", "?"),
                        member_or(row, "what", ""));
            }
        }

        return EXIT_SUCCESS;
    }

    /*
     * The operator's own tasks: one written down with nobody on it yet
     * (the backlog), or handed straight to an agent; assigned later; and
     * commented on, which the assignee hears if it has the work.
     */
    if (g_strcmp0(verb, "create") == 0) {
        g_autoptr(JsonBuilder) builder = json_builder_new();
        const gchar *prompt = NULL;
        JsonObject *task;
        gint arg;

        json_builder_begin_object(builder);

        for (arg = 3; arg < argc; arg++) {
            if ((g_strcmp0(argv[arg], "--agent") == 0 ||
                 g_strcmp0(argv[arg], "--title") == 0 ||
                 g_strcmp0(argv[arg], "--after") == 0 ||
                 g_strcmp0(argv[arg], "--review") == 0 ||
                 g_strcmp0(argv[arg], "--repo") == 0 ||
                 g_strcmp0(argv[arg], "--base") == 0 ||
                 g_strcmp0(argv[arg], "--project") == 0) && arg + 1 < argc) {
                json_builder_set_member_name(builder, argv[arg] + 2);

                /*
                 * A repository is a host path the daemon resolves, from
                 * its own working directory: made absolute here, from
                 * the one the person typed it in.
                 */
                if (g_strcmp0(argv[arg], "--repo") == 0) {
                    g_autofree gchar *absolute =
                        g_canonicalize_filename(argv[arg + 1], NULL);

                    json_builder_add_string_value(builder, absolute);
                } else {
                    json_builder_add_string_value(builder, argv[arg + 1]);
                }
                arg++;
            } else if (prompt == NULL && !g_str_has_prefix(argv[arg], "--")) {
                prompt = argv[arg];
            } else {
                prompt = NULL;
                break;
            }
        }

        if (prompt == NULL) {
            g_printerr("Usage: clawtilla task create [--agent AGENT] "
                       "[--title TITLE] [--after TASK,TASK] "
                       "[--review lead|delegator|operator|AGENT|none] "
                       "[--repo DIR [--base REF]] [--project ID] "
                       "<what to do>\n"
                       "Without --agent the task goes on the backlog; "
                       "assign it later with `clawtilla task assign`.\n");
            return EXIT_FAILURE;
        }

        json_builder_set_member_name(builder, "prompt");
        json_builder_add_string_value(builder, prompt);
        json_builder_end_object(builder);

        reply = call(client, "task.create", json_builder_get_root(builder));
        if (reply == NULL)
            return EXIT_FAILURE;

        task = json_object_get_object_member(json_node_get_object(reply),
                                             "task");

        if (json_object_has_member(task, "assignee"))
            g_print("Created %s and handed it to %s.\n",
                    member_or(task, "id", "?"),
                    member_or(task, "assignee", "?"));
        else
            g_print("Created %s on the backlog. Assign it with "
                    "`clawtilla task assign %s <agent>`.\n",
                    member_or(task, "id", "?"), member_or(task, "id", "?"));

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "assign") == 0) {
        if (argc < 5) {
            g_printerr("Usage: clawtilla task assign <task-id> <agent>\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "task.assign",
                     build_payload("task", argv[3], "agent", argv[4], NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("Assigned %s to %s.\n", argv[3], argv[4]);
        return EXIT_SUCCESS;
    }

    /*
     * What a task's checkout changed, as git prints it; or the checkout
     * taken away, its branch kept.
     */
    if (g_strcmp0(verb, "diff") == 0) {
        JsonArray *untracked;
        guint i;

        if (argc < 4) {
            g_printerr("Usage: clawtilla task diff <task-id>\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "task.diff",
                     build_payload("task", argv[3], NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("%s", member_or(json_node_get_object(reply), "diff", ""));

        untracked = json_object_has_member(json_node_get_object(reply),
                                           "untracked")
            ? json_object_get_array_member(json_node_get_object(reply),
                                           "untracked")
            : NULL;
        for (i = 0; untracked != NULL && i < json_array_get_length(untracked);
             i++)
            g_print("new, not yet added: %s\n",
                    json_array_get_string_element(untracked, i));

        if (json_object_get_boolean_member_with_default(
                json_node_get_object(reply), "truncated", FALSE))
            g_printerr("(cut short: the diff is larger than clawtilla "
                       "returns; read the rest in the checkout)\n");
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "checkout-remove") == 0) {
        gboolean force = (argc > 4 && g_strcmp0(argv[4], "--force") == 0);

        if (argc < 4) {
            g_printerr("Usage: clawtilla task checkout-remove <task-id> "
                       "[--force]\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "task.checkout_remove",
                     build_payload("task", argv[3],
                                   "force", force ? "true" : NULL, NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("Removed; its branch is kept in the repository.\n");
        return EXIT_SUCCESS;
    }

    /* The VENTURE mirror's Retry, for a task it skipped or gave up on. */
    if (g_strcmp0(verb, "venture-retry") == 0) {
        if (argc < 4) {
            g_printerr("Usage: clawtilla task venture-retry <task-id>\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "task.venture_retry",
                     build_payload("task", argv[3], NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("Queued for VENTURE again.\n");
        return EXIT_SUCCESS;
    }

    /* The operator's verdict on a result in review. */
    if (g_strcmp0(verb, "review") == 0) {
        if (argc < 5 || (g_strcmp0(argv[4], "accept") != 0 &&
                         g_strcmp0(argv[4], "changes") != 0) ||
            (g_strcmp0(argv[4], "changes") == 0 && argc < 6)) {
            g_printerr("Usage: clawtilla task review <task-id> accept\n"
                       "       clawtilla task review <task-id> changes "
                       "\"<what to change>\"\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "task.review",
                     build_payload("task", argv[3], "verdict", argv[4],
                                   "notes", argc > 5 ? argv[5] : NULL, NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("%s\n", member_or(json_node_get_object(reply), "said", ""));
        return EXIT_SUCCESS;
    }

    /* What a task waits for: added to, or taken away from. */
    if (g_strcmp0(verb, "link") == 0 || g_strcmp0(verb, "unlink") == 0) {
        gboolean linking = g_strcmp0(verb, "link") == 0;

        if (argc < 5) {
            g_printerr("Usage: clawtilla task %s <task-id> <task it waits "
                       "for>\n", verb);
            return EXIT_FAILURE;
        }

        reply = call(client, linking ? "task.link" : "task.unlink",
                     build_payload("task", argv[3], "after", argv[4], NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        g_print(linking ? "%s now waits for %s.\n"
                        : "%s no longer waits for %s.\n", argv[3], argv[4]);
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "comment") == 0) {
        JsonObject *root;

        if (argc < 5) {
            g_printerr("Usage: clawtilla task comment <task-id> <text>\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "task.comment",
                     build_payload("task", argv[3], "text", argv[4], NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        root = json_node_get_object(reply);

        if (json_object_get_boolean_member_with_default(root, "delivered",
                                                        FALSE))
            g_print("Noted on %s, and its assignee has it.\n", argv[3]);
        else
            g_print("Noted on %s. Nobody is working on it right now, so "
                    "it is kept with the task rather than sent.\n", argv[3]);

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

    /*
     * Sends a failed or stalled run round again: a task by its id, or an
     * incident (`inc-...`) from `task incidents`, which is how a routine
     * that never made a task is retried.
     */
    if (g_strcmp0(verb, "retry") == 0) {
        const gchar *key;

        if (argc < 4) {
            g_printerr("Usage: clawtilla task retry <task-id|incident-id> "
                       "[reason]\n");
            return EXIT_FAILURE;
        }

        key = g_str_has_prefix(argv[3], "inc-") ? "incident" : "task";

        reply = call(client, "task.retry",
                     build_payload(key, argv[3],
                                   "reason", (argc > 4) ? argv[4] : NULL,
                                   NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        if (json_object_get_boolean_member_with_default(
                json_node_get_object(reply), "rerun", FALSE))
            g_print("Running the routine again as task %s.\n",
                    member_or(json_node_get_object(reply), "task", "?"));
        else
            g_print("Task %s is queued for its assignee again.\n",
                    member_or(json_node_get_object(reply), "task", "?"));

        return EXIT_SUCCESS;
    }

    /* The open incidents: broken runs, and who was told. */
    if (g_strcmp0(verb, "incidents") == 0) {
        JsonObject *root;
        JsonArray *incidents;
        guint i;

        reply = call(client, "incident.list", NULL);
        if (reply == NULL)
            return EXIT_FAILURE;

        root = json_node_get_object(reply);
        incidents = json_object_get_array_member(root, "incidents");

        g_print("Reports go to: %s%s\n", member_or(root, "to", "lead"),
                json_object_get_boolean_member_with_default(root, "enabled",
                                                            TRUE)
                    ? "" : " (incidents are switched off)");

        if (incidents == NULL || json_array_get_length(incidents) == 0) {
            g_print("Nothing has broken.\n");
            return EXIT_SUCCESS;
        }

        g_print("%-26s %-12s %-16s %-26s %s\n", "ID", "AGENT", "KIND",
                "TASK", "TOLD");

        for (i = 0; i < json_array_get_length(incidents); i++) {
            JsonObject *incident = json_array_get_object_element(incidents,
                                                                 i);
            const gchar *told = member_or(incident, "recipient", NULL);

            g_print("%-26s %-12s %-16s %-26s %s%s\n",
                    member_or(incident, "id", "?"),
                    member_or(incident, "agent", "?"),
                    member_or(incident, "kind_label", "?"),
                    member_or(incident, "task",
                              member_or(incident, "routine", "-")),
                    told != NULL ? told : "nobody",
                    json_object_get_boolean_member_with_default(
                        incident, "queued", FALSE) ? " (waiting)" : "");
        }

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
static gboolean relay_policy_gate(const gchar *agent_id, const gchar *tool,
                                  const gchar *klass, gchar **refusal);

static gboolean
desktop_control_gate(const gchar *tool, JsonObject *arguments,
                     gchar **refusal, gpointer user_data)
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

    /* The agent's action rules first: a takeover is about now, a rule always. */
    if (!relay_policy_gate(agent_id, tool, "desktop_input", refusal))
        return FALSE;

    client = connect_to_daemon();

    if (client == NULL)
        return TRUE;

    /*
     * The arguments go too: the gate is the one place a desktop call is
     * seen, and saved steps record what was pressed, not only that
     * something was.
     */
    {
        g_autoptr(JsonBuilder) builder = json_builder_new();

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "agent");
        json_builder_add_string_value(builder, agent_id);
        json_builder_set_member_name(builder, "tool");
        json_builder_add_string_value(builder, tool);

        if (arguments != NULL) {
            JsonNode *copy = json_node_new(JSON_NODE_OBJECT);

            json_node_set_object(copy, arguments);
            json_builder_set_member_name(builder, "arguments");
            json_builder_add_value(builder, copy);
        }

        json_builder_end_object(builder);

        reply = clawt_client_request(client, "computer.control",
                                     json_builder_get_root(builder), &error);
    }

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

    /* The one verb about machines rather than an agent's machine. */
    if (g_strcmp0(verb, "shared") == 0) {
        JsonArray *computers;
        guint i;

        client = connect_to_daemon();
        if (client == NULL)
            return EXIT_FAILURE;
        reply = call(client, "computer.shared", NULL);
        if (reply == NULL)
            return EXIT_FAILURE;

        computers = json_object_has_member(json_node_get_object(reply), "computers")
            ? json_object_get_array_member(json_node_get_object(reply), "computers") : NULL;
        if (computers == NULL || json_array_get_length(computers) == 0) {
            g_print("No shared computers. Declare one under computers: and point "
                    "agents at it with computer.use.\n");
            return EXIT_SUCCESS;
        }

        for (i = 0; i < json_array_get_length(computers); i++) {
            JsonObject *computer = json_array_get_object_element(computers, i);
            JsonArray *users = json_object_get_array_member(computer, "users");
            JsonArray *running = json_object_get_array_member(computer, "running");
            guint u;

            g_print("%s  %s  %s%s%s\n  users:",
                    member_or(computer, "id", "?"), member_or(computer, "type", "?"),
                    member_or(computer, "state", "?"),
                    json_object_has_member(computer, "description") ? "  " : "",
                    member_or(computer, "description", ""));
            for (u = 0; users != NULL && u < json_array_get_length(users); u++) {
                const gchar *user = json_array_get_string_element(users, u);
                gboolean up = FALSE;
                guint r;

                for (r = 0; running != NULL && r < json_array_get_length(running); r++)
                    if (g_strcmp0(json_array_get_string_element(running, r), user) == 0)
                        up = TRUE;
                g_print(" %s%s", user, up ? " (running)" : "");
            }
            g_print("%s\n", users == NULL || json_array_get_length(users) == 0 ? " none" : "");
        }

        return EXIT_SUCCESS;
    }

    if (verb == NULL || agent_id == NULL) {
        g_printerr("Usage: clawtilla computer "
                   "<exec|status|screen|takeover|release|start|stop|restart|"
                   "rebuild|desktop-mcp> <agent> [-- COMMAND...]\n");
        g_printerr("       clawtilla computer shared\n");
        g_printerr("\n");
        g_printerr("  shared                 the machines agents share, and "
                   "who is on each\n");
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

        /*
         * Fifteen minutes rather than the usual two: a VM that starts on
         * first use is booted by this very request, and a first boot
         * runs cloud-init and installs a desktop before it answers.
         */
        {
            g_autoptr(GError) desktop_error = NULL;

            reply = clawt_client_request_full(
                client, "computer.desktop",
                build_payload("agent", agent_id, NULL), 900, &desktop_error);

            if (reply == NULL) {
                g_printerr("clawtilla: %s\n", desktop_error->message);
                return EXIT_FAILURE;
            }
        }

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

        /* A virtual display: which server, and what auto skipped. */
        {
            g_autofree gchar *display = clawt_display_status_line(screen);

            if (display != NULL)
                g_print("display:  %s\n", display);
        }

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

/* Parse the offline verb before connecting: validation must work when the
 * daemon cannot start because the very file being checked is broken. */
static gint
cmd_config_validate_file(int argc, char *argv[])
{
	g_autoptr(GOptionContext) context = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) warnings = NULL;
	g_autofree gchar *path = NULL;
	gboolean strict = FALSE;
	gboolean license = FALSE;
	gboolean valid;
	guint i;
	GOptionEntry options[] = {
		{ "file", 'f', 0, G_OPTION_ARG_FILENAME, &path,
		  "Validate an existing local YAML file without a daemon", "PATH" },
		{ "strict", 0, 0, G_OPTION_ARG_NONE, &strict,
		  "Treat warnings as errors (requires --file)", NULL },
		{ "license", 0, 0, G_OPTION_ARG_NONE, &license,
		  "Print the license", NULL },
		{ NULL }
	};

	context = g_option_context_new("--file PATH [--strict]");
	g_option_context_add_main_entries(context, options, NULL);
	g_option_context_set_description(context,
		"Examples:\n"
		"  clawtilla config validate --file candidate.yaml\n"
		"  clawtilla config validate --file candidate.yaml --strict\n\n"
		"Without options, validate checks the running daemon. Global -c does\n"
		"not select offline mode; use --file explicitly. No state is written.");
	argc -= 2;
	argv += 2;
	if (!g_option_context_parse(context, &argc, &argv, &error)) {
		g_printerr("clawtilla: %s\n", error->message);
		return EXIT_FAILURE;
	}
	if (license) {
		print_license();
		return EXIT_SUCCESS;
	}
	if (argc != 1 || path == NULL || *path == '\0') {
		g_printerr("Usage: clawtilla config validate --file PATH [--strict]\n");
		return EXIT_FAILURE;
	}
	valid = clawt_config_validate_file(path, strict, &warnings, &error);
	for (i = 0; warnings != NULL && i < warnings->len; i++)
		g_printerr("warning: %s\n",
		           (const gchar *)g_ptr_array_index(warnings, i));
	if (!valid) {
		g_printerr("invalid: %s\n", error->message);
		return EXIT_FAILURE;
	}
	g_print("Configuration is valid (%u warning(s)).\n", warnings->len);
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

	if (g_strcmp0(verb, "validate") == 0 && argc > 3)
		return cmd_config_validate_file(argc, argv);

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
            g_print("  clawtilla image vm get fedora-45  fetch one\n");
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
            g_printerr("  e.g. clawtilla image vm get fedora-45\n");
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
    g_printerr("  clawtilla image vm get fedora-45\n");
    g_printerr("  clawtilla image vm rm fedora-45 --force\n");
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
"  connect <name>          authorize, or sign in where the server says\n"
"  discover <name>         ask a remote server how to sign in to it\n"
"  key <name>              paste a token instead (read from stdin)\n"
"  refresh <name>          renew the credential now\n"
"  revoke <name>           forget it here and withdraw it there\n"
"  rm <name>               remove the connector entirely\n"
"\n"
"Options for add:\n"
"  --provider <id>         from `clawtilla connector catalog`; with only\n"
"                          --url, `remote` -- any MCP server by URL\n"
"  --account <label>       which account, in your words (work, personal);\n"
"                          required, and unique, for a second account\n"
"                          on the same service\n"
"  --client-id <id>        the OAuth app you registered with the provider\n"
"  --instance <url>        for a service you host yourself\n"
"  --scopes \"<a b>\"        override what to ask for\n"
"  --scope <all|selected|none>   which agents get it (default: none)\n"
"  --agents <a,b>          with --scope selected\n"
"  --command <cmd>         an MCP server of your own\n"
"  --url <url>             an HTTP MCP server of your own\n"
"  --tools <a,b>           narrow the agent to these tools only\n"
"  --apply-writes <off|all>  VENTURE: let the agent's writes land without\n"
"                          approval (default off: they wait for you)\n"
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
"  # Any remote MCP server: the URL is all it needs. The server says\n"
"  # where to sign in, and clawtilla registers itself there if it can:\n"
"  clawtilla connector add notion --url https://mcp.notion.com/mcp\n"
"  clawtilla connector connect notion\n"
"\n"
"  # A second account on the same service needs a label of its own:\n"
"  clawtilla connector add gh-home --provider github --account personal \\\n"
"      --command github-mcp-server --scope all\n"
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
        const gchar *server = member_or(root, "auth_server", NULL);

        /* Where, before the link: the person checks the host against it. */
        if (server != NULL)
            g_print("Signing in at %s\n\n", server);

        g_print("Open this to approve:\n\n    %s\n\n",
                member_or(root, "authorize_url", "?"));
    }

    g_print("\nWaiting...\n");

    /*
     * The link is useless until somebody reads it, and the wait below
     * can last minutes.  Piped -- a script, `tee`, a client running us
     * -- stdout is block-buffered, so without this the URL sat in the
     * buffer for the whole wait and appeared only after it had timed out.
     */
    fflush(stdout);

    return await_connector_flow(client, flow);
}

/*
 * The relay, which is what an agent's .mcp.json names.
 *
 * Nothing here may write to stdout: that is the MCP channel, and a
 * stray line of ours is a protocol error to the client on the other end
 * of it. Everything diagnostic goes to stderr.
 */
/*
 * Asks the agent's action rules about one call a relay is about to pass
 * on. Shared by the connector and desktop relays, which are started by the
 * agent's own CLI and so carry no token: policy.check takes the agent from
 * a local socket that is already the user's alone, and checking only ever
 * restricts the caller.
 *
 * A daemon that cannot be reached lets the call through, as the desktop
 * gate always has: the daemon supervises the agent, so an absent one is a
 * restart in progress, not a fleet without rules.
 */
static gboolean
relay_policy_gate_full(const gchar *agent_id, const gchar *tool,
                       const gchar *klass, const gchar *origin,
                       gchar **refusal);

static gboolean
relay_policy_gate(const gchar *agent_id, const gchar *tool, const gchar *klass,
                  gchar **refusal)
{
    return relay_policy_gate_full(agent_id, tool, klass, NULL, refusal);
}

/*
 * As relay_policy_gate(), saying who is asking: `app` for a call an MCP
 * app's view made, so the approval card says so and the daemon does not
 * count it against a turn the agent is running.
 */
static gboolean
relay_policy_gate_full(const gchar *agent_id, const gchar *tool,
                       const gchar *klass, const gchar *origin,
                       gchar **refusal)
{
    g_autoptr(ClawtClient) client = connect_to_daemon();
    g_autoptr(JsonNode) reply = NULL;
    JsonObject *result;

    if (client == NULL)
        return TRUE;

    /*
     * The room the CLI's session is in, so a connector write marks that
     * turn alone.  A relay started outside a turn has none -- the member
     * is then left out -- and the call counts against every running turn
     * (docs/mailboxes.org, "A turn a restart cut short").
     */
    {
        g_autofree gchar *summary = (origin != NULL)
            ? g_strdup_printf("%s, asked by an app in chat (not the model)",
                              tool)
            : g_strdup(tool);

        reply = clawt_client_request_full(
            client, "policy.check",
            build_payload("agent", agent_id, "tool", tool, "class", klass,
                          "summary", summary, "origin", origin,
                          "room", (origin != NULL) ? NULL
                                  : g_getenv("CLAWTILLA_ROOM_ID"),
                          NULL),
            3600, NULL);
    }
    if (reply == NULL || !JSON_NODE_HOLDS_OBJECT(reply))
        return TRUE;

    result = json_node_get_object(reply);
    if (json_object_get_boolean_member_with_default(result, "allowed", TRUE))
        return TRUE;

    *refusal = g_strdup(json_object_get_string_member_with_default(
        result, "reason", "Refused by an action rule."));
    return FALSE;
}

static gboolean
connector_policy_gate(const gchar *tool, JsonObject *arguments,
                      gchar **refusal, gpointer user_data)
{
    (void)arguments;

    *refusal = NULL;
    return relay_policy_gate(user_data, tool, "external", refusal);
}

/* The same rules for a call an MCP app's view made, said as the app's. */
static gboolean
connector_app_policy_gate(const gchar *tool, JsonObject *arguments,
                          gchar **refusal, gpointer user_data)
{
    (void)arguments;

    *refusal = NULL;
    return relay_policy_gate_full(user_data, tool, "external", "app",
                                  refusal);
}

/*
 * Tells the daemon a remote server answered 401, so every client can
 * offer the sign-in.  Best effort: the agent has already been told, in
 * words, what to do -- a daemon that cannot be reached loses only the
 * button, never the explanation.
 */
static void
connector_unauthorized(const gchar *name, const gchar *challenge,
                       gboolean sent_credential, gpointer user_data)
{
    g_autoptr(ClawtClient) client = connect_to_daemon();
    g_autoptr(JsonNode) ignored = NULL;

    if (client == NULL)
        return;

    ignored = clawt_client_request(
        client, "connector.unauthorized",
        build_payload("name", name, "challenge", challenge, NULL), NULL);
}

/*
 * Tells the daemon a result was trimmed, so the conversation shows a
 * notice where the turn's tool calls are.  Best effort, like the rest of
 * these: the agent was told in the result itself, which is what matters.
 */
static void
connector_result_trimmed(const gchar *connector, const gchar *tool,
                         gsize original_bytes, gsize kept_bytes,
                         const gchar *result_id, gpointer user_data)
{
    const gchar *agent_id = user_data;
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) ignored = NULL;
    g_autoptr(JsonBuilder) builder = NULL;
    const gchar *room = g_getenv("CLAWTILLA_ROOM_ID");

    if (agent_id == NULL || (client = connect_to_daemon()) == NULL)
        return;

    builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, connector != NULL ? connector : "");
    json_builder_set_member_name(builder, "agent");
    json_builder_add_string_value(builder, agent_id);

    if (room != NULL && *room != '\0') {
        json_builder_set_member_name(builder, "room");
        json_builder_add_string_value(builder, room);
    }

    if (tool != NULL) {
        json_builder_set_member_name(builder, "tool");
        json_builder_add_string_value(builder, tool);
    }

    json_builder_set_member_name(builder, "original_bytes");
    json_builder_add_int_value(builder, (gint64)original_bytes);
    json_builder_set_member_name(builder, "kept_bytes");
    json_builder_add_int_value(builder, (gint64)kept_bytes);

    if (result_id != NULL) {
        json_builder_set_member_name(builder, "result_id");
        json_builder_add_string_value(builder, result_id);
    }

    json_builder_end_object(builder);

    ignored = clawt_client_request(client, "connector.result_trimmed",
                                   json_builder_get_root(builder), NULL);
}

/*
 * Hands the daemon one call of an app's tool, view and all.  The relay
 * is the only thing that can read the view -- it holds the credential
 * and the connection -- so it is read here and kept there.
 */
static void
connector_app_seen(JsonObject *report, gpointer user_data)
{
    const gchar *agent_id = user_data;
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) ignored = NULL;
    g_autoptr(JsonObject) payload = NULL;
    JsonNode *node;
    g_autoptr(GError) error = NULL;
    const gchar *room = g_getenv("CLAWTILLA_ROOM_ID");

    if (agent_id == NULL || (client = connect_to_daemon()) == NULL)
        return;

    payload = json_object_new();
    json_object_set_string_member(payload, "agent", agent_id);

    if (room != NULL && *room != '\0')
        json_object_set_string_member(payload, "room", room);

    json_object_set_object_member(payload, "app", json_object_ref(report));

    /* The request takes the node: transfer full. */
    node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, payload);

    ignored = clawt_client_request(client, "connector.app", node, &error);

    if (ignored == NULL && error != NULL)
        g_printerr("clawtilla: could not hand the daemon an app: %s\n",
                   error->message);
}

/* Whether a remote server is on this machine, which lets its views reach it. */
static gboolean
url_is_loopback(const gchar *url)
{
    g_autoptr(GUri) uri = NULL;
    const gchar *host;

    if (url == NULL ||
        (uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL)) == NULL)
        return FALSE;

    host = g_uri_get_host(uri);

    return g_strcmp0(host, "localhost") == 0 ||
           g_strcmp0(host, "127.0.0.1") == 0 || g_strcmp0(host, "::1") == 0;
}

typedef struct {
    const gchar *name;
    const gchar *agent_id;

    /* What was last reported, so an unchanged list is not sent again. */
    gint64       tools;
    gboolean     active;
} CatalogueReport;

/*
 * Tells the daemon how big the server turned out to be.
 *
 * The daemon never starts a connector's server itself -- the relay does,
 * with the credential -- so this is the only place the count exists.
 * Best effort: a daemon that is not there loses a number on a settings
 * page, not a tool call, and nothing is written to stdout, which is the
 * MCP channel.
 */
static void
on_connector_catalogued(ClawtMcpDirectory *directory, gpointer user_data)
{
    CatalogueReport *report = user_data;
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) ignored = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonBuilder) builder = NULL;
    g_autofree gchar *areas = NULL;
    gint64 tools = clawt_mcp_directory_get_tool_count(directory);
    gboolean active = clawt_mcp_directory_is_active(directory);

    if (tools == report->tools && active == report->active)
        return;

    report->tools = tools;
    report->active = active;

    client = connect_to_daemon();

    if (client == NULL)
        return;

    areas = clawt_mcp_directory_overview(directory, 2000);
    builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, report->name);

    if (report->agent_id != NULL) {
        json_builder_set_member_name(builder, "agent");
        json_builder_add_string_value(builder, report->agent_id);
    }

    json_builder_set_member_name(builder, "tools");
    json_builder_add_int_value(builder, tools);
    json_builder_set_member_name(builder, "chars");
    json_builder_add_int_value(
        builder, (gint64)clawt_mcp_directory_get_char_count(directory));
    json_builder_set_member_name(builder, "areas");
    json_builder_add_string_value(builder, areas);
    json_builder_set_member_name(builder, "directory");
    json_builder_add_boolean_value(builder, active);
    json_builder_end_object(builder);

    ignored = clawt_client_request(client, "connector.tools_seen",
                                   json_builder_get_root(builder), &error);

    if (ignored == NULL && error != NULL)
        g_printerr("clawtilla: connector '%s': could not report its tool "
                   "count: %s\n", report->name, error->message);
}

static gint
connector_relay(const gchar *name, const gchar *agent_id, gboolean app_call)
{
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(ClawtIntegrationBinding) binding = NULL;
    g_autoptr(ClawtConnectorPlan) plan = NULL;
    g_autoptr(ClawtMcpDirectory) directory = NULL;
    g_autoptr(GError) error = NULL;
    CatalogueReport report = { NULL, NULL, -1, FALSE };

    if (name == NULL) {
        g_printerr("clawtilla: connector relay needs a name\n");
        return EXIT_FAILURE;
    }

    /* An app's call is asked of an agent's rules, so it needs the agent. */
    if (app_call && agent_id == NULL) {
        g_printerr("clawtilla: connector relay --app-call needs --agent\n");
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

    /*
     * The same resolution the daemon makes for its own calls: the
     * instance, its provider, and the token file where the daemon keeps
     * the credential now -- one function, so the two reach the server
     * the same way.  No credential yet is still a plan for a server
     * reached by URL: its 401 is what asks for a sign-in.
     */
    plan = clawt_connector_plan_for_name(config, name, &binding, &error);

    if (plan == NULL) {
        g_printerr("clawtilla: %s\n", error->message);
        return EXIT_FAILURE;
    }

    plan->unauthorized = connector_unauthorized;

    /*
     * Several accounts on one service: this account's tools carry its
     * label, decided from every connector the agent holds -- the same
     * answer each sibling relay reaches on its own.
     */
    if (agent_id != NULL) {
        ClawtAgentConfig *agent = clawt_config_get_agent(config, agent_id);

        if (agent != NULL) {
            g_autoptr(GPtrArray) bindings =
                clawt_integration_resolve_for_agent(config, agent);
            g_autoptr(GError) conflict = NULL;
            guint i;

            for (i = 0; bindings != NULL && i < bindings->len; i++) {
                ClawtIntegrationBinding *mine = g_ptr_array_index(bindings,
                                                                  i);

                if (g_strcmp0(clawt_integration_binding_get_name(mine),
                              name) != 0)
                    continue;

                plan->account_prefix = clawt_connector_account_prefix(
                    bindings, mine, &plan->account_siblings, &conflict);
                plan->accounts = plan->account_prefix != NULL ||
                                 conflict != NULL;

                if (conflict != NULL) {
                    g_printerr("clawtilla: %s\n", conflict->message);
                    plan->account_conflict = g_strdup(conflict->message);
                }

                break;
            }
        }
    }

    /*
     * The directory is always in the path: it presents a big server as
     * three tools when the connector and the agent's CLI call for it,
     * and otherwise only counts what goes past -- which is how the
     * Connectors page and TOOLS.org learn how big a server is.  With
     * several accounts it is named for this one, and so is every tool
     * name it shows.
     */
    /*
     * The relay the daemon starts for an MCP app's own call
     * (connector.app_call): the same plan, credential, granted list and
     * action rules -- through connector_app_policy_gate(), which says
     * the app is asking -- with the server's own tool names (the app is
     * the server's code and knows no account prefix), no directory in
     * front, no result budget (the daemon bounds what it reads), and
     * apps turned around so only tools visible to apps may be called.
     */
    if (app_call) {
        if (!clawt_integration_binding_get_boolean(binding, "show_apps")) {
            g_printerr("clawtilla: connector '%s' does not show its apps in "
                       "chat\n", name);
            return EXIT_FAILURE;
        }

        plan->accounts = FALSE;
        g_clear_pointer(&plan->account_prefix, g_free);
        plan->apps = clawt_mcp_apps_new(
            name, plan->argv != NULL || url_is_loopback(plan->url));
        clawt_mcp_apps_set_app_caller(plan->apps, TRUE);

        return clawt_connector_relay_run_full(plan, connector_app_policy_gate,
                                              (gpointer)agent_id, NULL);
    }

    /*
     * The result budget: the connector's own, else the fleet's.  Whole
     * results are kept only for an agent -- a relay run by hand has
     * nobody to read them back.
     */
    {
        gint64 budget = clawt_mcp_result_budget_resolve(
            clawt_integration_binding_get_int(binding, "max_result_bytes"),
            clawt_config_get_int(config, "connectors.max_result_bytes"));
        g_autofree gchar *spill_dir = (agent_id != NULL)
            ? clawt_mcp_result_spill_dir(config, agent_id) : NULL;

        plan->results = clawt_mcp_results_new(name, (gsize)budget,
                                              spill_dir);
        clawt_mcp_results_set_trimmed_func(plan->results,
                                           connector_result_trimmed,
                                           (gpointer)agent_id);
    }

    /* Apps, only for a connector somebody switched them on for. */
    if (clawt_integration_binding_get_boolean(binding, "show_apps")) {
        plan->apps = clawt_mcp_apps_new(
            name, plan->argv != NULL || url_is_loopback(plan->url));
        clawt_mcp_apps_set_seen_func(plan->apps, connector_app_seen,
                                     (gpointer)agent_id);
    }

    directory = clawt_connector_directory_new(config, binding, agent_id);
    clawt_mcp_directory_set_account(directory, plan->account_prefix);
    report.name = name;
    report.agent_id = agent_id;
    g_signal_connect(directory, "catalogued",
                     G_CALLBACK(on_connector_catalogued), &report);

    return clawt_connector_relay_run_full(
        plan, (agent_id != NULL) ? connector_policy_gate : NULL,
        (gpointer)agent_id, directory);
}

static void
print_broker_usage(gboolean asked_for)
{
    static const gchar *text =
"Usage: clawtilla broker <command>\n"
"\n"
"The integration broker clawtilla runs in podman when broker.enabled is on:\n"
"AnythingMCP (broker.type: anythingmcp) or Jentic One (jentic). Each agent\n"
"or team names its gateway server in broker_server and reaches it as the\n"
"connector broker-agent-<id> or broker-team-<id>.\n"
"\n"
"Commands:\n"
"  status                  whether it runs, its dashboard, who reaches it\n"
"  start                   start it (answers at once; status says starting)\n"
"  stop                    stop it; its data under <state_dir>/broker stays\n"
"  restart                 stop it, then start it\n"
"  logs                    the last hundred lines of each container\n"
"  key <agent|team:<id>>   store the key for that gateway server (stdin)\n"
"\n"
"Examples:\n"
"  clawtilla config edit     # set broker.enabled: true; saving reloads\n"
"  clawtilla broker status\n"
"\n"
"  # In the dashboard: make an MCP server for scout, assign connectors to\n"
"  # it, make a key on its page. Then:\n"
"  clawtilla agent set scout broker_server cmwgug12p000535o2f9egw8iy\n"
"  clawtilla broker key scout\n"
"\n"
"  # One server for a whole team:\n"
"  clawtilla broker key team:research\n"
"\n"
"  clawtilla broker logs\n"
"  clawtilla broker restart\n";

    print_usage_text(text, asked_for);
}

/* The status reply, as lines a person reads. */
static void
print_broker_status(JsonObject *root)
{
    JsonArray *assignments;
    JsonArray *warnings;
    guint i;

    g_print("%s: %s", member_or(root, "label", "broker"),
            member_or(root, "state_label", "?"));

    if (json_object_has_member(root, "detail"))
        g_print(" -- %s", member_or(root, "detail", ""));

    g_print("\n");

    if (!json_object_get_boolean_member_with_default(root, "enabled",
                                                     FALSE)) {
        g_print("broker.enabled is off. Set `broker.enabled: true` with "
                "`clawtilla config edit`; the reload it ends with starts it.\n");
        return;
    }

    g_print("  image:     %s\n", member_or(root, "image", "?"));
    g_print("  dashboard: %s\n", member_or(root, "dashboard_url", "?"));
    g_print("  mcp:       %s\n", member_or(root, "mcp_url", "?"));

    if (json_object_get_int_member_with_default(root, "restarts", 0) > 0)
        g_print("  restarts:  %" G_GINT64_FORMAT "\n",
                json_object_get_int_member_with_default(root, "restarts",
                                                        0));

    /* How the last container to stop ended, and what broker.restart did. */
    if (json_object_has_member(root, "last_exit") &&
        JSON_NODE_HOLDS_OBJECT(json_object_get_member(root, "last_exit"))) {
        JsonObject *last = json_object_get_object_member(root, "last_exit");

        if (last != NULL)
            g_print("  last exit: %s %s; %s\n",
                    member_or(last, "container", "?"),
                    member_or(last, "how", "stopped"),
                    json_object_get_boolean_member_with_default(
                        last, "restarted", FALSE)
                    ? "restarted" : "not restarted");
    }

    if (json_object_has_member(root, "refusal"))
        g_print("  refused:   %s\n", member_or(root, "refusal", ""));

    assignments = json_object_has_member(root, "assignments")
                  ? json_object_get_array_member(root, "assignments") : NULL;

    if (assignments == NULL || json_array_get_length(assignments) == 0) {
        g_print("  No agent names a gateway server yet: set broker_server "
                "on an agent or a team.\n");
    } else {
        g_print("  Gateway servers:\n");

        for (i = 0; i < json_array_get_length(assignments); i++) {
            JsonObject *one = json_array_get_object_element(assignments, i);
            JsonArray *agents = json_object_get_array_member(one, "agents");
            g_autoptr(GString) names = g_string_new(NULL);
            guint a;

            for (a = 0; agents != NULL && a < json_array_get_length(agents);
                 a++) {
                if (a > 0)
                    g_string_append(names, ", ");
                g_string_append(names,
                                json_array_get_string_element(agents, a));
            }

            g_print("    %-20s server %s, connector %s, %s -- %s\n",
                    member_or(one, "target", "?"),
                    member_or(one, "server", "?"),
                    member_or(one, "connector", "?"),
                    json_object_get_boolean_member_with_default(
                        one, "key_stored", FALSE)
                    ? "key stored" : "no key yet (clawtilla broker key)",
                    names->str);
        }
    }

    warnings = json_object_has_member(root, "warnings")
               ? json_object_get_array_member(root, "warnings") : NULL;

    for (i = 0; warnings != NULL && i < json_array_get_length(warnings); i++)
        g_print("  warning: %s\n", json_array_get_string_element(warnings, i));
}

static gint
cmd_broker(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    const gchar *verb = (argc > 2) ? argv[2] : "status";

    if (g_strcmp0(verb, "help") == 0 || g_strcmp0(verb, "--help") == 0 ||
        g_strcmp0(verb, "-h") == 0) {
        print_broker_usage(TRUE);
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "status") != 0 && g_strcmp0(verb, "start") != 0 &&
        g_strcmp0(verb, "stop") != 0 && g_strcmp0(verb, "restart") != 0 &&
        g_strcmp0(verb, "logs") != 0 && g_strcmp0(verb, "key") != 0) {
        g_printerr("clawtilla: unknown broker command '%s'\n", verb);
        print_broker_usage(FALSE);
        return EXIT_FAILURE;
    }

    client = connect_to_daemon();

    if (client == NULL)
        return EXIT_FAILURE;

    if (g_strcmp0(verb, "key") == 0) {
        const gchar *target = (argc > 3) ? argv[3] : NULL;
        g_autofree gchar *key = NULL;

        if (target == NULL) {
            g_printerr("clawtilla: broker key needs an agent, or team:<id>\n");
            return EXIT_FAILURE;
        }

        /*
         * From stdin with the echo off, like connector key, and for the
         * same reason: an argument is in the shell history and the
         * process table.
         */
        key = read_secret("Key: ");

        if (key == NULL || *key == '\0') {
            g_printerr("clawtilla: no key was given\n");
            return EXIT_FAILURE;
        }

        reply = call(client, "broker.key",
                     build_payload("target", target, "key", key, NULL));

        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("Stored in %s\n",
                member_or(json_node_get_object(reply), "key_file", "?"));
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "logs") == 0) {
        reply = call(client, "broker.logs", NULL);

        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("%s", member_or(json_node_get_object(reply), "logs", ""));
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "start") == 0)
        reply = call(client, "broker.start", NULL);
    else if (g_strcmp0(verb, "stop") == 0)
        reply = call(client, "broker.stop", NULL);
    else if (g_strcmp0(verb, "restart") == 0)
        reply = call(client, "broker.restart", NULL);
    else
        reply = call(client, "broker.status", NULL);

    if (reply == NULL)
        return EXIT_FAILURE;

    print_broker_status(json_node_get_object(reply));

    return EXIT_SUCCESS;
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
    if (g_strcmp0(verb, "relay") == 0) {
        const gchar *agent_id = NULL;
        gboolean app_call = FALSE;
        gint arg;

        for (arg = 4; arg < argc; arg++) {
            if (g_strcmp0(argv[arg], "--agent") == 0 && arg + 1 < argc)
                agent_id = argv[++arg];
            else if (g_strcmp0(argv[arg], "--app-call") == 0)
                app_call = TRUE;
        }

        return connector_relay(name, agent_id, app_call);
    }

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

            /*
             * A server that refused us is worth more than the expiry: the
             * credential may look fine and still be one it will not take.
             */
            if (json_object_get_boolean_member_with_default(
                    entry, "needs_sign_in", FALSE) ||
                (!connected && json_object_get_boolean_member_with_default(
                                   entry, "sign_in", FALSE))) {
                g_free(status);
                status = g_strdup_printf(
                    "sign in needed%s%s -- `clawtilla connector connect %s`",
                    member_or(entry, "auth_server", NULL) != NULL
                    ? " at " : "",
                    member_or(entry, "auth_server", ""),
                    member_or(entry, "name", "?"));
            }

            {
                const gchar *account = member_or(entry, "account", NULL);
                g_autofree gchar *provider = (account != NULL)
                    ? g_strdup_printf("%s/%s",
                                      member_or(entry, "provider", "?"),
                                      account)
                    : g_strdup(member_or(entry, "provider", "?"));

                g_print("%-14s %-18s %-10s %s\n",
                        member_or(entry, "name", "?"), provider,
                        member_or(entry, "scope", ""), status);
            }

            /* How many tools, and whether agents get them as a directory. */
            {
                JsonObject *root = json_node_get_object(reply);
                g_autofree gchar *tools = clawt_mcp_directory_status_label(
                    member_or(entry, "directory", "auto"),
                    json_object_get_int_member_with_default(entry,
                                                            "tool_count", -1),
                    json_object_get_int_member_with_default(entry,
                                                            "tool_chars", 0),
                    json_object_get_int_member_with_default(
                        root, "directory_min_tools", 40),
                    json_object_get_int_member_with_default(
                        root, "directory_min_chars", 100000));

                g_print("%-14s tools: %s\n", "", tools);
            }
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
        g_strcmp0(verb, "discover") != 0 &&
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
        gboolean have_url = FALSE;
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

            if (g_strcmp0(key, "url") == 0)
                have_url = TRUE;

            if (g_strcmp0(key, "client_id") == 0)
                client_id = argv[a + 1];

            json_builder_set_member_name(builder, key);
            json_builder_add_string_value(builder, argv[a + 1]);
        }

        json_builder_end_object(builder);

        /* A URL alone is a remote server; the daemon fills the provider. */
        if (!have_provider && !have_url) {
            g_printerr("clawtilla: connector add needs --provider, or --url "
                       "for a remote MCP server; `clawtilla connector "
                       "catalog` lists the providers\n");
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

        /*
         * A server reached by URL is asked how to sign in to it now --
         * the person is here and has just asked for this server, which is
         * the moment the network is wanted.  Asked for a catalogue entry
         * too, not only for --url: one that names its server (higgsfield)
         * discovers its sign-in the same way, and "needs --client-id" was
         * the wrong advice for it.  The daemon answers without the
         * network for a provider that is not reached by URL.
         */
        {
            g_autoptr(JsonNode) found = call(client, "connector.discover",
                                             build_payload("name", name,
                                                           NULL));

            if (found != NULL &&
                json_object_get_boolean_member_with_default(
                    json_node_get_object(found), "sign_in", FALSE)) {
                g_print("It signs in at %s.\n",
                        member_or(json_node_get_object(found), "auth_server",
                                  "?"));
                return connector_connect(client, name);
            }
        }

        g_print("\nNow give it a credential, either:\n"
                "  clawtilla connector key %s        (paste a token)\n"
                "  clawtilla connector connect %s    (needs --client-id)\n",
                name, name);

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "connect") == 0)
        return connector_connect(client, name);

    if (g_strcmp0(verb, "discover") == 0) {
        JsonObject *root;

        reply = call(client, "connector.discover",
                     build_payload("name", name, NULL));

        if (reply == NULL)
            return EXIT_FAILURE;

        root = json_node_get_object(reply);

        if (!json_object_get_boolean_member_with_default(root, "sign_in",
                                                         FALSE)) {
            g_print("No sign-in found: %s\n",
                    member_or(root, "reason", "the server publishes none"));
            return EXIT_SUCCESS;
        }

        g_print("Signs in at %s (%s)%s.\n"
                "Run `clawtilla connector connect %s` to sign in.\n",
                member_or(root, "auth_server", "?"),
                member_or(root, "source", "?"),
                json_object_get_boolean_member_with_default(
                    root, "registration", FALSE)
                ? "; clawtilla can register itself there" : "",
                name);
        return EXIT_SUCCESS;
    }

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
        "  proposals                  skills agents have proposed, waiting\n"
        "  approve <proposal>         apply one: enable it, or write the update\n"
        "  reject <proposal> [why]    discard one; the agent is told why\n"
        "  tests <name>               its saved tests, and how each last ran\n"
        "  test <name> [test] [--agent AGENT]\n"
        "                             run them (or one) on an agent with it\n"
        "\n"
        "An imported skill lands disabled and its scripts are left behind.\n"
        "Read it, then enable it: what a skill says goes into a model's\n"
        "context with your agent's own authority.\n"
        "\n"
        "Examples:\n"
        "  clawtilla skill import ~/src/some-repo/skills/release\n"
        "  clawtilla skill show release\n"
        "  clawtilla skill enable release\n"
        "  clawtilla skill assign release team:engineering\n"
        "  clawtilla skill proposals\n"
        "  clawtilla skill reject skill-proposal-1f0c... \"too specific to one repo\"\n"
        "  clawtilla skill test release --agent builder\n";

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

    /* A skill's saved tests, and each one's last run. */
    if (g_strcmp0(verb, "tests") == 0 && name != NULL) {
        JsonObject *object;
        JsonArray *tests;
        JsonArray *problems;

        reply = call(client, "skill.tests",
                     build_payload("skill", name, NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        object = json_node_get_object(reply);
        tests = json_object_get_array_member(object, "tests");
        problems = json_object_get_array_member(object, "problems");

        if (json_array_get_length(tests) == 0)
            g_print("%s has no tests. Add tests/<name>.yaml beside its "
                    "SKILL.md: input, and expect / reject.\n", name);

        for (i = 0; i < json_array_get_length(tests); i++) {
            JsonObject *test = json_array_get_object_element(tests, i);
            JsonObject *last = json_object_has_member(test, "last")
                ? json_object_get_object_member(test, "last") : NULL;

            g_print("%-20s %-8s %s\n", member_or(test, "name", ""),
                    last != NULL ? member_or(last, "state", "") : "never",
                    last != NULL ? member_or(last, "detail", "") : "");
        }

        for (i = 0; i < json_array_get_length(problems); i++)
            g_print("problem: %s\n",
                    json_array_get_string_element(problems, i));
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "test") == 0 && name != NULL) {
        const gchar *test = NULL;
        const gchar *agent = NULL;
        JsonArray *runs;
        gint arg;

        for (arg = 4; arg < argc; arg++) {
            if (g_strcmp0(argv[arg], "--agent") == 0 && arg + 1 < argc)
                agent = argv[++arg];
            else if (test == NULL)
                test = argv[arg];
        }

        reply = call(client, "skill.test_run",
                     build_payload("skill", name, "test", test,
                                   "agent", agent, NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        runs = json_object_get_array_member(json_node_get_object(reply),
                                            "runs");
        for (i = 0; i < json_array_get_length(runs); i++) {
            JsonObject *run = json_array_get_object_element(runs, i);

            /* on a scratch copy of the agent named, since 0.50.0 */
            if (json_object_has_member(run, "as"))
                g_print("%s: running as %s on %s (a scratch copy of %s)\n",
                        member_or(run, "test", ""), member_or(run, "task", ""),
                        member_or(run, "agent", ""), member_or(run, "as", ""));
            else
                g_print("%s: running as %s on %s\n",
                        member_or(run, "test", ""), member_or(run, "task", ""),
                        member_or(run, "agent", ""));
        }
        g_print("`clawtilla skill tests %s` shows the verdicts once they "
                "finish.\n", name);
        return EXIT_SUCCESS;
    }

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

    if (g_strcmp0(verb, "proposals") == 0) {
        JsonArray *proposals;

        reply = call(client, "skill.proposals", NULL);
        if (reply == NULL)
            return EXIT_FAILURE;

        proposals = json_object_get_array_member(json_node_get_object(reply),
                                                 "proposals");

        if (proposals == NULL || json_array_get_length(proposals) == 0) {
            g_print("No skill proposals are waiting.\n");
            return EXIT_SUCCESS;
        }

        /*
         * The text the agent wrote arrives escaped, and the full draft or
         * diff is printed so the person approving it from a terminal has
         * read what they are approving.
         */
        for (i = 0; i < json_array_get_length(proposals); i++) {
            JsonObject *proposal = json_array_get_object_element(proposals, i);
            const gchar *kind = member_or(proposal, "kind", "create");
            const gchar *diff = member_or(proposal, "diff", "");

            g_print("%s\n  %s %s '%s'\n",
                    member_or(proposal, "id", "?"),
                    member_or(proposal, "agent", "?"),
                    g_strcmp0(kind, "update") == 0 ? "proposes a change to"
                                                  : "proposes a new skill",
                    member_or(proposal, "name", "?"));

            if (*member_or(proposal, "reason", "") != '\0')
                g_print("  why: %s\n", member_or(proposal, "reason", ""));
            if (json_object_get_boolean_member_with_default(proposal, "stale", FALSE))
                g_print("  ! the file changed since it was proposed; approve is refused\n");

            g_print("\n%s\n", g_strcmp0(kind, "update") == 0 && *diff != '\0'
                                  ? diff : member_or(proposal, "text", ""));
        }

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "approve") == 0 || g_strcmp0(verb, "reject") == 0) {
        g_autoptr(JsonBuilder) builder = json_builder_new();

        if (name == NULL) {
            g_printerr("Usage: clawtilla skill %s <proposal>%s\n", verb,
                       g_strcmp0(verb, "reject") == 0 ? " [why]" : "");
            return EXIT_FAILURE;
        }

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "proposal");
        json_builder_add_string_value(builder, name);
        json_builder_set_member_name(builder, "action");
        json_builder_add_string_value(builder, verb);
        if (argc > 4) {
            json_builder_set_member_name(builder, "note");
            json_builder_add_string_value(builder, argv[4]);
        }
        json_builder_end_object(builder);

        reply = call(client, "skill.proposal_resolve",
                     json_builder_get_root(builder));
        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("%s %s.\n", name,
                g_strcmp0(verb, "approve") == 0 ? "approved" : "rejected");

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

            /*
             * Off because its computer would not start, which "(off)"
             * alone cannot tell apart from somebody switching it off --
             * so why, in libclawt's words, and how to turn it back on.
             */
            if (json_object_has_member(routine, "turned_off_after")) {
                g_autofree gchar *note = clawt_routine_turned_off_note(
                    (guint)json_object_get_int_member(routine,
                                                      "turned_off_after"),
                    member_or(routine, "turned_off_error", NULL));

                g_printerr("  %s: %s\n"
                           "  turn it back on: clawtilla routine set %s "
                           "enabled=true\n",
                           member_or(routine, "id", "?"), note,
                           member_or(routine, "id", "?"));
            }
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
print_grok_usage(gboolean asked_for)
{
    static const gchar *text =
        "Usage: clawtilla grok <verb> [...]\n"
        "\n"
        "  bots                           the Grok bots, who can reach each,\n"
        "                                 and whether a job could be sent now\n"
        "  jobs [--bot B] [--agent A] [--open]\n"
        "                                 work handed to Grok bots, newest first\n"
        "  send <bot> <instructions...> --agent A [--context TEXT]\n"
        "       [--timeout MINUTES]       hand a job to a bot on an agent's\n"
        "                                 behalf; its answer goes to that agent\n"
        "  test <bot> --agent A           a ping job; the bot should answer pong\n"
        "  routine-prompt <bot>           the instructions to paste into the\n"
        "                                 bot's routine in Grok, once\n"
        "  mcp-key <bot>                  mint the key that connects the bot to\n"
        "                                 clawtilla's MCP endpoint; printed ONCE\n"
        "  remove <job>                   cancel an open job (its agent is told)\n"
        "                                 or forget a settled one\n"
        "\n"
        "A bot is a `grokbot` integration named for it, whose url and token\n"
        "are a Grok routine's webhook address and key. The bot reports back\n"
        "to this daemon's webhook listener over the tailnet, so\n"
        "daemon.webhook_enabled and daemon.tailscale must both be on.\n"
        "\n"
        "Examples:\n"
        "  clawtilla integration add chief grokbot \\\n"
        "      url=https://api2.cursor.sh/automations/webhook/... \\\n"
        "      token=file:~/.clawtilla/secrets/grok-chief.key \\\n"
        "      description=\"Chief of staff: calendar, mail, errands\" \\\n"
        "      scope=all\n"
        "  clawtilla grok routine-prompt chief | wl-copy\n"
        "  clawtilla grok mcp-key chief\n"
        "  clawtilla grok test chief --agent oryx\n"
        "  clawtilla grok send chief --agent oryx \\\n"
        "      \"Find three venues near the office for Thursday\"\n"
        "  clawtilla grok jobs --open\n"
        "  clawtilla grok remove gj-70d186db8fb7\n";

    print_usage_text(text, asked_for);
}

/* A value after a --flag, or NULL; and the flag's own index, consumed. */
static const gchar *
flag_value(int argc, char *argv[], const gchar *flag, GHashTable *used)
{
    int i;

    for (i = 3; i + 1 < argc; i++) {
        if (g_strcmp0(argv[i], flag) == 0) {
            g_hash_table_add(used, GINT_TO_POINTER(i));
            g_hash_table_add(used, GINT_TO_POINTER(i + 1));
            return argv[i + 1];
        }
    }

    return NULL;
}

static gboolean
flag_present(int argc, char *argv[], const gchar *flag, GHashTable *used)
{
    int i;

    for (i = 3; i < argc; i++) {
        if (g_strcmp0(argv[i], flag) == 0) {
            g_hash_table_add(used, GINT_TO_POINTER(i));
            return TRUE;
        }
    }

    return FALSE;
}

/* ── Files sent in a conversation ──────────────────────────────────── */

static void
print_attachment_usage(gboolean asked_for)
{
    static const gchar *text =
        "Usage: clawtilla attachment <verb> [...]\n"
        "\n"
        "  list <agent|room>              every file a conversation carried,\n"
        "                                 newest first: id, kind, size, who\n"
        "                                 sent it and when\n"
        "  get <id> [-o FILE]             the daemon's copy of one, written to\n"
        "                                 FILE (default: its own name, in the\n"
        "                                 current directory); -o - for stdout\n"
        "\n"
        "An agent id lists your conversation with it; a room id lists that\n"
        "room. A file named only by a host path (a message from before\n"
        "0.51.5) is listed with that path and has no id to get.\n"
        "\n"
        "Examples:\n"
        "  clawtilla attachment list scout\n"
        "  clawtilla attachment get 0f3c9a1b2c3d-report.pdf\n"
        "  clawtilla attachment get 0f3c9a1b2c3d-shot.png -o /tmp/shot.png\n";

    if (asked_for)
        g_print("%s", text);
    else
        g_printerr("%s", text);
}

static int
cmd_attachment(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(GHashTable) used = g_hash_table_new(NULL, NULL);
    const gchar *verb = (argc > 2) ? argv[2] : "help";
    const gchar *what = (argc > 3 && argv[3][0] != '-') ? argv[3] : NULL;
    guint i;

    if (g_strcmp0(verb, "help") == 0 || g_strcmp0(verb, "--help") == 0 ||
        g_strcmp0(verb, "-h") == 0) {
        print_attachment_usage(TRUE);
        return EXIT_SUCCESS;
    }

    if ((g_strcmp0(verb, "list") != 0 && g_strcmp0(verb, "get") != 0) ||
        what == NULL) {
        print_attachment_usage(FALSE);
        return EXIT_FAILURE;
    }

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    if (g_strcmp0(verb, "list") == 0) {
        JsonArray *items;

        /*
         * `room`, which the daemon resolves as a room id first and an
         * agent's direct room second -- the same pair room.history takes.
         */
        reply = call(client, "attachment.list",
                     clawt_ipc_payload_new("room", what, NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        items = json_object_get_array_member(json_node_get_object(reply),
                                             "items");

        if (items == NULL || json_array_get_length(items) == 0) {
            g_print("No files in that conversation yet.\n");
            return EXIT_SUCCESS;
        }

        for (i = 0; i < json_array_get_length(items); i++) {
            JsonObject *item = json_array_get_object_element(items, i);
            gint64 ts = json_object_get_int_member_with_default(item, "ts",
                                                                0);
            gint64 bytes = json_object_get_int_member_with_default(
                item, "bytes", 0);
            g_autoptr(GDateTime) when = ts > 0
                ? g_date_time_new_from_unix_local(ts) : NULL;
            g_autofree gchar *date = when != NULL
                ? g_date_time_format(when, "%Y-%m-%d %H:%M") : g_strdup("?");
            g_autofree gchar *size = g_format_size((guint64)bytes);

            g_print("%-6s %-10s %-16s %s  %s\n",
                    member_or(item, "kind", "other"),
                    member_flag(item, "exists", FALSE) ? size : "gone",
                    member_or(item, "sender", "?"), date,
                    member_or(item, "name", "?"));
            g_print("    %s\n", member_or(item, "id", NULL) != NULL
                                    ? member_or(item, "id", "")
                                    : member_or(item, "path", ""));
        }

        return EXIT_SUCCESS;
    }

    /* get */
    {
        const gchar *output = flag_value(argc, argv, "-o", used);
        const gchar *encoded;
        g_autofree gchar *name = NULL;
        g_autofree guchar *bytes = NULL;
        g_autoptr(GError) error = NULL;
        gsize length = 0;

        reply = call(client, "attachment.get",
                     clawt_ipc_payload_new("id", what, NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        encoded = member_or(json_node_get_object(reply), "base64", NULL);

        if (encoded == NULL) {
            g_printerr("clawtilla: the daemon sent no bytes\n");
            return EXIT_FAILURE;
        }

        bytes = g_base64_decode(encoded, &length);

        if (g_strcmp0(output, "-") == 0) {
            if (fwrite(bytes, 1, length, stdout) != length) {
                g_printerr("clawtilla: could not write to stdout\n");
                return EXIT_FAILURE;
            }

            return EXIT_SUCCESS;
        }

        /*
         * Its own name by default -- the daemon's, which it rebuilt from
         * a basename, so it cannot name a path outside this directory.
         */
        name = g_path_get_basename(
            member_or(json_node_get_object(reply), "name", what));

        if (!g_file_set_contents(output != NULL ? output : name,
                                 (const gchar *)bytes, (gssize)length,
                                 &error)) {
            g_printerr("clawtilla: %s\n", error->message);
            return EXIT_FAILURE;
        }

        g_print("Wrote %s (%" G_GSIZE_FORMAT " bytes).\n",
                output != NULL ? output : name, length);
    }

    return EXIT_SUCCESS;
}

static void
print_grok_job(JsonObject *job, gint64 now)
{
    gint64 created = 0;
    g_autofree gchar *age = NULL;
    gint state = CLAWT_GROK_JOB_FAILED;
    const gchar *result = member_or(job, "result", NULL);
    const gchar *note = member_or(job, "note", NULL);

    if (json_object_has_member(job, "created_at"))
        created = json_object_get_int_member(job, "created_at");

    age = clawt_time_ago_label(created, now);
    clawt_enum_from_nick(CLAWT_TYPE_GROK_JOB_STATE,
                         member_or(job, "state", "failed"), &state);

    g_print("%-16s %-12s %-14s %-12s %s\n", member_or(job, "id", "?"),
            member_or(job, "bot", "?"),
            clawt_grok_job_state_label((ClawtGrokJobState)state),
            member_or(job, "agent", "-"), age);
    g_print("    asked: %s\n", member_or(job, "instructions", ""));

    if (member_or(job, "run_uuid", NULL) != NULL)
        g_print("    grok run: %s\n", member_or(job, "run_uuid", ""));

    if (note != NULL && !member_flag(job, "settled", FALSE))
        g_print("    latest: %s\n", note);

    if (result != NULL && *result != '\0')
        g_print("    %s: %s\n",
                state == CLAWT_GROK_JOB_REFUSED ? "why" : "answer", result);
}

static int
cmd_grok(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(GHashTable) used = g_hash_table_new(NULL, NULL);
    const gchar *verb = (argc > 2) ? argv[2] : "bots";
    const gchar *bot = (argc > 3 && argv[3][0] != '-') ? argv[3] : NULL;
    gint64 now = g_get_real_time();
    guint i;

    if (g_strcmp0(verb, "help") == 0 || g_strcmp0(verb, "--help") == 0 ||
        g_strcmp0(verb, "-h") == 0) {
        print_grok_usage(TRUE);
        return EXIT_SUCCESS;
    }

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    if (g_strcmp0(verb, "bots") == 0) {
        JsonObject *root;
        JsonArray *bots;

        reply = call(client, "grok.bots", NULL);
        if (reply == NULL)
            return EXIT_FAILURE;

        root = json_node_get_object(reply);
        bots = json_object_get_array_member(root, "bots");

        if (member_flag(root, "reachable", FALSE))
            g_print("Bots report back to %s.\n\n",
                    member_or(root, "callback_host", "?"));
        else
            g_print("No job can be sent right now: %s.\n\n",
                    member_or(root, "unreachable_reason", "unknown"));

        if (bots == NULL || json_array_get_length(bots) == 0) {
            g_print("No Grok bot is shared with any agent. Add a `grokbot` "
                    "integration: `clawtilla grok help`.\n");
            return EXIT_SUCCESS;
        }

        for (i = 0; i < json_array_get_length(bots); i++) {
            JsonObject *one = json_array_get_object_element(bots, i);
            JsonArray *agents = json_object_get_array_member(one, "agents");
            g_autoptr(GString) who = g_string_new(NULL);
            guint a;

            for (a = 0; agents != NULL && a < json_array_get_length(agents);
                 a++) {
                if (who->len > 0)
                    g_string_append(who, ", ");
                g_string_append(who,
                                json_array_get_string_element(agents, a));
            }

            g_print("%-14s %s\n", member_or(one, "name", "?"),
                    member_or(one, "description", ""));
            g_print("    reachable by: %s\n", who->str);
            g_print("    key: %s   mcp: %s\n",
                    member_flag(one, "key_configured", FALSE) ? "set"
                                                              : "MISSING",
                    member_flag(one, "mcp_key", FALSE)
                        ? "connected" : "not connected (grok mcp-key)");
        }

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "jobs") == 0) {
        g_autoptr(JsonBuilder) builder = json_builder_new();
        const gchar *only_bot = flag_value(argc, argv, "--bot", used);
        const gchar *agent = flag_value(argc, argv, "--agent", used);
        JsonArray *jobs;

        json_builder_begin_object(builder);
        if (only_bot != NULL) {
            json_builder_set_member_name(builder, "bot");
            json_builder_add_string_value(builder, only_bot);
        }
        if (agent != NULL) {
            json_builder_set_member_name(builder, "agent");
            json_builder_add_string_value(builder, agent);
        }
        json_builder_set_member_name(builder, "open");
        json_builder_add_boolean_value(builder,
                                       flag_present(argc, argv, "--open",
                                                    used));
        json_builder_end_object(builder);

        reply = call(client, "grok.jobs", json_builder_get_root(builder));
        if (reply == NULL)
            return EXIT_FAILURE;

        jobs = json_object_get_array_member(json_node_get_object(reply),
                                            "jobs");

        if (jobs == NULL || json_array_get_length(jobs) == 0) {
            g_print("No Grok jobs%s.\n",
                    only_bot != NULL || agent != NULL ? " match" : " yet");
            return EXIT_SUCCESS;
        }

        for (i = 0; i < json_array_get_length(jobs); i++)
            print_grok_job(json_array_get_object_element(jobs, i), now);

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "remove") == 0) {
        g_autoptr(JsonBuilder) builder = json_builder_new();

        if (bot == NULL) {
            g_printerr("clawtilla: say which job: its id, from `clawtilla "
                       "grok jobs`\n");
            print_grok_usage(FALSE);
            return EXIT_FAILURE;
        }

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "job");
        json_builder_add_string_value(builder, bot);
        json_builder_end_object(builder);

        reply = call(client, "grok.remove", json_builder_get_root(builder));
        if (reply == NULL)
            return EXIT_FAILURE;

        if (member_flag(json_node_get_object(reply), "cancelled", FALSE))
            g_print("Cancelled %s; its agent was told to stop waiting.\n",
                    bot);
        else
            g_print("Removed %s.\n", bot);

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "send") == 0 || g_strcmp0(verb, "test") == 0) {
        g_autoptr(JsonBuilder) builder = json_builder_new();
        g_autoptr(GString) instructions = g_string_new(NULL);
        const gchar *agent = flag_value(argc, argv, "--agent", used);
        const gchar *context = flag_value(argc, argv, "--context", used);
        const gchar *timeout = flag_value(argc, argv, "--timeout", used);
        gboolean test = g_strcmp0(verb, "test") == 0;
        int a;

        if (bot == NULL || agent == NULL) {
            g_printerr("clawtilla: say which bot, and --agent for whose job "
                       "it is: the answer goes to that agent\n");
            print_grok_usage(FALSE);
            return EXIT_FAILURE;
        }

        if (test) {
            g_string_append(instructions,
                "This is a connection test from clawtilla. Do nothing else: "
                "report this job straight away with status done and result "
                "\"pong\".");
        } else {
            for (a = 4; a < argc; a++) {
                if (g_hash_table_contains(used, GINT_TO_POINTER(a)))
                    continue;
                if (instructions->len > 0)
                    g_string_append_c(instructions, ' ');
                g_string_append(instructions, argv[a]);
            }
        }

        if (instructions->len == 0) {
            g_printerr("clawtilla: what should the bot do?\n");
            return EXIT_FAILURE;
        }

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "bot");
        json_builder_add_string_value(builder, bot);
        json_builder_set_member_name(builder, "agent");
        json_builder_add_string_value(builder, agent);
        json_builder_set_member_name(builder, "instructions");
        json_builder_add_string_value(builder, instructions->str);
        if (context != NULL) {
            json_builder_set_member_name(builder, "context");
            json_builder_add_string_value(builder, context);
        }
        if (timeout != NULL || test) {
            json_builder_set_member_name(builder, "timeout_minutes");
            json_builder_add_int_value(builder,
                                       timeout != NULL
                                           ? g_ascii_strtoll(timeout, NULL,
                                                             10)
                                           : 10);
        }
        json_builder_end_object(builder);

        reply = call(client, "grok.send", json_builder_get_root(builder));
        if (reply == NULL)
            return EXIT_FAILURE;

        {
            JsonObject *job = json_object_get_object_member(
                json_node_get_object(reply), "job");

            g_print("Started job %s on %s (Grok run %s).\n",
                    member_or(job, "id", "?"), member_or(job, "bot", "?"),
                    member_or(job, "run_uuid", "unknown"));
            g_print("The answer goes to %s as a message. Follow it with "
                    "`clawtilla grok jobs --bot %s`.\n", agent,
                    member_or(job, "bot", bot));
        }

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "routine-prompt") == 0) {
        if (bot == NULL) {
            print_grok_usage(FALSE);
            return EXIT_FAILURE;
        }

        reply = call(client, "grok.routine_prompt",
                     build_payload("bot", bot, NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        g_print("%s", member_or(json_node_get_object(reply), "text", ""));
        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "mcp-key") == 0) {
        JsonObject *root;

        if (bot == NULL) {
            print_grok_usage(FALSE);
            return EXIT_FAILURE;
        }

        reply = call(client, "grok.mcp_key", build_payload("bot", bot, NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        root = json_node_get_object(reply);

        g_print("In Grok, ask the bot to add a remote MCP connection:\n");
        g_print("  URL:      %s\n", member_or(root, "mcp_url",
                                              "(not reachable yet -- see "
                                              "`clawtilla grok bots`)"));
        g_print("  API key:  %s\n", member_or(root, "key", "?"));
        g_print("\nThis is the only time the key is shown. Minting another "
                "replaces it.\nThen run `clawtilla grok routine-prompt %s` "
                "again: it now names the MCP route.\n", bot);
        return EXIT_SUCCESS;
    }

    g_printerr("clawtilla: unknown grok verb '%s'\n", verb);
    print_grok_usage(FALSE);
    return EXIT_FAILURE;
}

static void
print_trigger_usage(gboolean asked_for)
{
    g_autoptr(GString) providers = g_string_new(NULL);
    g_autoptr(GString) modes = g_string_new(NULL);
    g_autofree gchar *text = NULL;
    guint p;

    /*
     * Walked from the library, so a provider added there is named here
     * the moment it exists -- the list in this help had already missed
     * one before it was made to.
     */
    for (p = 0; p < clawt_trigger_provider_count(); p++) {
        if (providers->len > 0)
            g_string_append(providers, ", ");

        g_string_append(providers, clawt_trigger_provider_nth_nick(p));
    }

    /* The modes too, from the same kind of table and for the same reason. */
    for (p = 0; p < clawt_trigger_mode_count(); p++) {
        if (modes->len > 0)
            g_string_append(modes, ", ");

        g_string_append_printf(modes, "%s (%s)",
                               clawt_trigger_mode_nth_nick(p),
                               clawt_trigger_mode_nth_label(p));
    }

    text = g_strdup_printf(
        "Usage: clawtilla trigger <verb> [...]\n"
        "\n"
        "  list                           what can start a run, and where\n"
        "  add <id> <agent> [key=value]   add one; prints the secret ONCE\n"
        "  set <id> [key=value ...]       change one\n"
        "  rm <id>                        remove it\n"
        "  rotate <id>                    new secret and new address\n"
        "  test <id> [--run]              show the prompt, or run it\n"
        "  replay <id> [receipt] [--run]  explain a stored event; explicitly replay once\n"
        "  capture <id>                   the first delivery, as it arrived\n"
        "  check <id> [path=value ...]    try a match filter on the newest\n"
        "                                 delivery, and list its fields\n"
        "  deliveries [id]                what has been sent, and what came\n"
        "                                 of it\n"
        "\n"
        "Providers: %s.\n"
        "Modes: %s.\n"
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
        "  clawtilla trigger deliveries ci-failed\n"
        "\n"
        "  # Only pull requests whose title says release, and only opened\n"
        "  # ones; repeat match= for each pair (at most 10, all must match,\n"
        "  # ignoring case), and match= on its own clears the filter.\n"
        "  clawtilla trigger set review match=pull_request.title=release \\\n"
        "      match=action=opened\n"
        "\n"
        "  # Post deploy notices into a room without running anybody, and\n"
        "  # let one trigger have at most two runs unfinished.\n"
        "  clawtilla trigger set deploys mode=post room=ops\n"
        "  clawtilla trigger set ci-failed max_pending=2\n"
        "\n"
        "  # VENTURE: create its webhook with this trigger's secret as the\n"
        "  # signing secret; ignore the agent's own writes; catch up on\n"
        "  # business events through the 'books' connector.\n"
        "  clawtilla trigger add paid bookkeeper provider=venture \\\n"
        "      events=invoice.paid ignore_actors=bookkeeper-bot \\\n"
        "      replay_connector=books organization_id=1 \\\n"
        "      instructions=\"{{kind}} {{number}} was paid. Reconcile it.\"\n",
        providers->str, modes->str);

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

/* A partial fan-out failure must stay visible even when its primary ran. */
static gint
print_trigger_results(JsonObject *root)
{
    JsonArray *results;
    guint i;
    gboolean failed = FALSE;

    /* A post trigger ran nothing, and says where it wrote instead. */
    if (json_object_has_member(root, "posted")) {
        g_print("Posted into %s. No agent was run.\n",
                member_or(root, "posted", "?"));
        return EXIT_SUCCESS;
    }

    if (!json_object_has_member(root, "results")) {
        g_print("Started as task %s.\n", member_or(root, "task", "?"));
        return EXIT_SUCCESS;
    }
    results = json_object_get_array_member(root, "results");
    for (i = 0; i < json_array_get_length(results); i++) {
        JsonObject *row = json_array_get_object_element(results, i);
        gboolean bad = json_object_has_member(row, "error");
        failed |= bad;
        g_print("%s: %s %s\n", member_or(row, "agent", "?"),
                bad ? "failed:" : "task", member_or(row, bad ? "error" : "task", "?"));
    }
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
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
        g_autoptr(GPtrArray) match_pairs = NULL;
        gboolean match_given = FALSE;

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

            /*
             * `match` is a list of pairs, and a pair is `path=value`, so
             * it is given once per pair rather than comma-joined -- a
             * value may well hold a comma.  Gathered here and sent once,
             * because a JSON object naming `match` twice keeps one.
             */
            if (entry->type == CLAWT_SCHEMA_LIST_OF) {
                match_given = TRUE;

                if (match_pairs == NULL)
                    match_pairs = g_ptr_array_new_with_free_func(g_free);

                if (*parts[1] != '\0')
                    g_ptr_array_add(match_pairs, g_strdup(parts[1]));

                continue;
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

        if (match_given) {
            guint m;

            json_builder_set_member_name(builder, "match");

            if (match_pairs->len == 0) {
                json_builder_add_null_value(builder);
            } else {
                json_builder_begin_array(builder);

                for (m = 0; m < match_pairs->len; m++)
                    json_builder_add_string_value(
                        builder, g_ptr_array_index(match_pairs, m));

                json_builder_end_array(builder);
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

        if (run)
            return print_trigger_results(root);

        g_print("%s\n", member_or(root, "report", member_or(root, "prompt", "")));

        if (g_strcmp0(member_or(root, "report", ""), "") != 0 &&
            g_str_has_prefix(member_or(root, "report", ""), "Posts into:"))
            g_print("\nThat is what would be posted; no agent runs. "
                    "`--run` posts it.\n");
        else
            g_print("\nThat is what the agent would be asked. "
                    "`--run` sends it.\n");

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "replay") == 0) {
        g_autoptr(JsonBuilder) builder = json_builder_new();
        gint64 receipt = 0;
        gboolean run = FALSE;
        JsonObject *root;

        /* Pin execution to a receipt shown by the preview. Latest is
         * useful for inspection but could change before a subsequent run. */
        if (id == NULL) {
            g_printerr("Usage: clawtilla trigger replay <id> [receipt] [--run]\n");
            return EXIT_FAILURE;
        }
        for (i = 4; i < (guint)argc; i++) {
            if (g_strcmp0(argv[i], "--run") == 0)
                run = TRUE;
            else if (receipt != 0 ||
                     !g_ascii_string_to_signed(argv[i], 10, 1, G_MAXINT64, &receipt, NULL)) {
                g_printerr("Expected a positive receipt number or --run.\n");
                return EXIT_FAILURE;
            }
        }
        if (run && receipt == 0) {
            g_printerr("Preview first, then name its receipt number with --run.\n");
            return EXIT_FAILURE;
        }
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "id");
        json_builder_add_string_value(builder, id);
        json_builder_set_member_name(builder, "receipt");
        json_builder_add_int_value(builder, receipt);
        json_builder_set_member_name(builder, "run");
        json_builder_add_boolean_value(builder, run);
        json_builder_end_object(builder);
        reply = call(client, "trigger.replay", json_builder_get_root(builder));
        if (reply == NULL)
            return EXIT_FAILURE;
        root = json_node_get_object(reply);
        g_print("%s\n", member_or(root, "report", ""));
        if (run)
            return print_trigger_results(root);
        else
            g_print("\nPreview only. Repeat with the receipt number and --run to execute once.\n");
        return EXIT_SUCCESS;
    }

    /*
     * A match filter against the newest body this trigger received --
     * the saved one, or pairs given here to try before saving them.
     */
    if (g_strcmp0(verb, "check") == 0) {
        g_autoptr(JsonBuilder) builder = json_builder_new();
        JsonObject *root;
        JsonArray *rows;
        int a;

        if (id == NULL) {
            g_printerr("Usage: clawtilla trigger check <id> [path=value ...]\n");
            return EXIT_FAILURE;
        }

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "id");
        json_builder_add_string_value(builder, id);

        if (argc > 4) {
            json_builder_set_member_name(builder, "match");
            json_builder_begin_array(builder);

            for (a = 4; a < argc; a++)
                json_builder_add_string_value(builder, argv[a]);

            json_builder_end_array(builder);
        }

        json_builder_end_object(builder);

        reply = call(client, "trigger.match_test",
                     json_builder_get_root(builder));

        if (reply == NULL)
            return EXIT_FAILURE;

        root = json_node_get_object(reply);

        if (!json_object_has_member(root, "payload")) {
            g_print("%s\n", member_or(root, "note", "nothing to check against"));
            return EXIT_SUCCESS;
        }

        rows = json_object_get_array_member(root, "results");

        for (i = 0; i < json_array_get_length(rows); i++) {
            JsonObject *row = json_array_get_object_element(rows, i);

            g_print("%-5s %s contains '%s'%s%s\n",
                    member_flag(row, "matched", FALSE) ? "ok" : "MISS",
                    member_or(row, "path", "?"), member_or(row, "value", ""),
                    member_flag(row, "found", FALSE) ? " -- it is " : " -- nothing there",
                    member_flag(row, "found", FALSE) ? member_or(row, "actual", "") : "");
        }

        g_print("\n%s\n", member_flag(root, "matches", FALSE)
                ? "The newest delivery would have been let through."
                : member_or(root, "reason", "The newest delivery would have been ignored."));

        rows = json_object_get_array_member(root, "paths");

        if (json_array_get_length(rows) > 0) {
            g_print("\nFields in it:\n");

            for (i = 0; i < json_array_get_length(rows); i++) {
                JsonObject *row = json_array_get_object_element(rows, i);

                g_print("  %-40s %s\n", member_or(row, "path", "?"),
                        member_or(row, "value", ""));
            }
        }

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

        g_print("%-8s %-20s %-16s %-14s %-10s %s\n", "RECEIPT", "WHEN", "TRIGGER", "EVENT",
                "OUTCOME", "DETAIL");

        for (i = 0; i < json_array_get_length(deliveries); i++) {
            JsonObject *row = json_array_get_object_element(deliveries, i);
            gint64 at = g_ascii_strtoll(member_or(row, "at", "0"), NULL, 10);
            g_autoptr(GDateTime) when = g_date_time_new_from_unix_local(at);
            g_autofree gchar *stamp =
                (when != NULL) ? g_date_time_format(when, "%a %d %b %H:%M")
                               : g_strdup("-");

            g_print("%-8s %-20s %-16s %-14s %-10s %s\n",
                    member_or(row, "receipt", "?"), stamp,
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
     * Anything else after `daemon` is not a request to run one.  Every
     * other command answers --help with its usage, so `clawtilla daemon
     * --help` was run expecting the same -- and instead started a daemon
     * against the default config, rewriting its state directory and
     * binding the tailnet listener, for a reader who only wanted to know
     * what the verb takes.
     */
    if (argc > 2) {
        gboolean asked = g_strcmp0(argv[2], "--help") == 0 ||
                         g_strcmp0(argv[2], "-h") == 0 ||
                         g_strcmp0(argv[2], "help") == 0;

        static const gchar usage[] =
            "Usage: clawtilla daemon [token]\n"
            "\n"
            "  (none)   run the daemon in the foreground, as clawtillad does\n"
            "  token    print the token a remote client must present\n";

        if (asked) {
            g_print("%s", usage);
            return EXIT_SUCCESS;
        }
        g_printerr("clawtilla: unknown daemon verb '%s'\n%s", argv[2], usage);
        return EXIT_FAILURE;
    }

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
            g_print("  clawtilla remote pair <name> <host> <code>   "
                    "(code from `clawtilla device pair`)\n");
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

    /*
     * The easy path: a six-digit code from `clawtilla device pair` on the
     * other machine, traded for a token of this device's own and saved
     * here, 0600.  Nothing to copy across by hand, and nothing in the
     * shell history that opens the daemon.
     */
    if (g_strcmp0(verb, "pair") == 0) {
        const gchar *name = (argc > 3) ? argv[3] : NULL;
        const gchar *address = (argc > 4) ? argv[4] : NULL;
        const gchar *code = (argc > 5) ? argv[5] : NULL;
        const gchar *device_name = NULL;
        g_autofree gchar *host = NULL;
        guint16 port = CLAWT_DEFAULT_TCP_PORT;
        gboolean tls = FALSE;
        gboolean insecure = FALSE;
        ClawtConnection *connection;
        gint arg;

        if (name == NULL || address == NULL || code == NULL) {
            g_printerr("Usage: clawtilla remote pair <name> <host[:port]> "
                       "<code> [--device-name N] [--tls] [--insecure]\n");
            g_printerr("  e.g. clawtilla remote pair workstation "
                       "100.72.0.41 123456\n");
            g_printerr("  `clawtilla device pair` on that machine prints "
                       "the code.\n");
            return EXIT_FAILURE;
        }

        for (arg = 6; arg < argc; arg++) {
            if (g_strcmp0(argv[arg], "--device-name") == 0 &&
                arg + 1 < argc)
                device_name = argv[++arg];
            else if (g_strcmp0(argv[arg], "--tls") == 0)
                tls = TRUE;
            else if (g_strcmp0(argv[arg], "--insecure") == 0)
                insecure = TRUE;
        }

        if (!clawt_ipc_parse_listen_address(address, CLAWT_DEFAULT_TCP_PORT,
                                            &host, &port, &error)) {
            g_printerr("clawtilla: %s\n", error->message);
            return EXIT_FAILURE;
        }

        if (clawt_connection_list_find(connections, name) != NULL) {
            g_printerr("clawtilla: there is already a connection called "
                       "'%s'\n", name);
            return EXIT_FAILURE;
        }

        connection = clawt_connection_new_paired(name, host, port, tls,
                                                 insecure, code, device_name,
                                                 &error);

        if (connection == NULL) {
            g_printerr("clawtilla: %s\n", error->message);
            return EXIT_FAILURE;
        }

        g_ptr_array_add(connections, connection);

        if (!clawt_connection_list_save(NULL, connections, &error)) {
            g_printerr("clawtilla: paired, but the token could not be "
                       "saved: %s\n", error->message);
            g_printerr("  Revoke the new device on that machine "
                       "(`clawtilla device list`) and try again.\n");
            return EXIT_FAILURE;
        }

        g_print("Paired and saved %s. Try it:\n"
                "  clawtilla --profile %s status\n", name, name);

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

    g_printerr("Usage: clawtilla remote {list,add,pair,rm}\n");

    return EXIT_FAILURE;
}

/* ── device ──────────────────────────────────────────────────────── */

static void
print_device_usage(gboolean asked_for)
{
    static const gchar *text =
        "Usage: clawtilla device [list|pair|revoke] [ARGS...]\n"
        "\n"
        "  list                  paired devices: id, name, when paired, when\n"
        "                        last seen -- never a token\n"
        "  pair [--name NAME] [--qr [--qr-light]]\n"
        "                        print a six-digit code another device trades\n"
        "                        for a token of its own (five minutes, single\n"
        "                        use, five wrong guesses and it is gone);\n"
        "                        --qr also draws the pairing link as a QR code\n"
        "                        for a phone to scan, and prints the link\n"
        "                        itself; --qr-light draws it for a terminal\n"
        "                        with a light background\n"
        "  revoke ID             stop that device's token working, and close\n"
        "                        whatever it has connected now (the owner\n"
        "                        only; a paired device may revoke itself,\n"
        "                        which signs it out)\n"
        "\n"
        "The daemon's own token (`clawtilla daemon token`) stays the owner\n"
        "credential: revoking or expiring devices never touches it. Only the\n"
        "owner can pair and revoke -- so a stolen phone cannot cut off the\n"
        "owner's laptop -- but any paired device can sign itself out by\n"
        "revoking its own id, and any connected client can list.\n"
        "daemon.device_idle_days expires a device nobody has used for that\n"
        "long (0 = never).\n"
        "\n"
        "Examples:\n"
        "  # On the machine running the fleet\n"
        "  clawtilla device pair --name phone\n"
        "\n"
        "  # The same, with a QR code for the phone's camera\n"
        "  clawtilla device pair --name phone --qr\n"
        "\n"
        "  # On the device being paired, with the address and code it printed\n"
        "  clawtilla remote pair workstation 100.72.0.41 123456\n"
        "\n"
        "  clawtilla device list\n"
        "  clawtilla device revoke d3f9a01bc\n";

    print_usage_text(text, asked_for);
}

static gint
cmd_device(int argc, char *argv[])
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    const gchar *verb = (argc > 2) ? argv[2] : "list";

    if (g_strcmp0(verb, "-h") == 0 || g_strcmp0(verb, "--help") == 0 ||
        g_strcmp0(verb, "help") == 0) {
        print_device_usage(TRUE);
        return EXIT_SUCCESS;
    }

    /* Refused before connecting: answering a typo needs no daemon. */
    if (g_strcmp0(verb, "list") != 0 && g_strcmp0(verb, "pair") != 0 &&
        g_strcmp0(verb, "revoke") != 0) {
        g_printerr("clawtilla: unknown device verb '%s'\n", verb);
        print_device_usage(FALSE);
        return EXIT_FAILURE;
    }

    if (g_strcmp0(verb, "revoke") == 0 && argc < 4) {
        g_printerr("Usage: clawtilla device revoke ID\n"
                   "  `clawtilla device list` shows the ids.\n");
        return EXIT_FAILURE;
    }

    client = connect_to_daemon();
    if (client == NULL)
        return EXIT_FAILURE;

    if (g_strcmp0(verb, "pair") == 0) {
        const gchar *name = NULL;
        const gchar *uri;
        gboolean qr = FALSE;
        gboolean qr_light = FALSE;
        JsonObject *payload;
        JsonArray *addresses;
        gint arg;
        guint i;

        for (arg = 3; arg < argc; arg++) {
            if (g_strcmp0(argv[arg], "--name") == 0 && arg + 1 < argc)
                name = argv[++arg];
            else if (g_strcmp0(argv[arg], "--qr") == 0)
                qr = TRUE;
            else if (g_strcmp0(argv[arg], "--qr-light") == 0)
                qr = qr_light = TRUE;
            else {
                g_printerr("clawtilla: unexpected argument '%s'\n",
                           argv[arg]);
                return EXIT_FAILURE;
            }
        }

        reply = call(client, "device.pair", build_payload("name", name,
                                                          NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        payload = json_node_get_object(reply);
        addresses = json_object_has_member(payload, "addresses")
            ? json_object_get_array_member(payload, "addresses") : NULL;
        uri = json_object_get_string_member_with_default(payload, "uri",
                                                         NULL);

        /*
         * The QR code above the code, so the screen reads top to bottom
         * in the order somebody uses it: scan, or else type.  The link
         * is printed whether or not a code could be drawn -- a phone
         * that cannot scan this terminal can still be sent the text.
         */
        if (qr && uri != NULL) {
            g_autoptr(GError) qr_error = NULL;
            g_autofree gchar *art = clawt_qr_render_terminal(uri, qr_light,
                                                             &qr_error);

            if (art != NULL)
                g_print("%s\n", art);
            else
                g_print("No QR code: %s.\n\n", qr_error->message);

            g_print("Pairing link:  %s\n", uri);
        } else if (qr) {
            g_print("No QR code: the daemon named no address another device "
                    "could dial.\n\n");
        }

        g_print("Pairing code:  %s\n",
                json_object_get_string_member(payload, "display"));
        g_print("Good for %" G_GINT64_FORMAT " minutes, once.\n\n",
                json_object_get_int_member(payload, "expires_in") / 60);

        /*
         * Where to dial, because a code is no use without an address and
         * this is the one moment somebody is guaranteed to be looking.
         */
        if (addresses == NULL || json_array_get_length(addresses) == 0) {
            g_print("This daemon is not listening on the network, so no other "
                    "device can reach it\nto redeem this. Turn on "
                    "daemon.tailscale or daemon.tcp_enabled, or start it\n"
                    "with --bind.\n");
            return EXIT_SUCCESS;
        }

        g_print("On the other device, enter the address and the code under "
                "\"Pair with a code\"\nin Connections, or run:\n\n");

        for (i = 0; i < json_array_get_length(addresses); i++)
            g_print("  clawtilla remote pair %s %s %s\n",
                    name != NULL ? name : "NAME",
                    json_array_get_string_element(addresses, i),
                    json_object_get_string_member(payload, "code"));

        return EXIT_SUCCESS;
    }

    if (g_strcmp0(verb, "revoke") == 0) {
        JsonObject *payload;

        reply = call(client, "device.revoke", build_payload("id", argv[3],
                                                            NULL));
        if (reply == NULL)
            return EXIT_FAILURE;

        payload = json_node_get_object(reply);

        g_print("Revoked %s.", argv[3]);

        if (json_object_get_int_member(payload, "closed") > 0)
            g_print(" Closed %" G_GINT64_FORMAT " live connection(s).",
                    json_object_get_int_member(payload, "closed"));

        g_print("\n");

        return EXIT_SUCCESS;
    }

    /* list */
    {
        JsonObject *payload;
        JsonArray *devices;
        gint64 now = g_get_real_time() / G_USEC_PER_SEC;
        guint i;

        reply = call(client, "device.list", NULL);
        if (reply == NULL)
            return EXIT_FAILURE;

        payload = json_node_get_object(reply);
        devices = json_object_get_array_member(payload, "devices");

        if (devices == NULL || json_array_get_length(devices) == 0) {
            g_print("No paired devices.\n"
                    "  clawtilla device pair --name phone\n");
            return EXIT_SUCCESS;
        }

        g_print("%-11s %-22s %-17s %s\n", "ID", "NAME", "PAIRED",
                "LAST SEEN");

        for (i = 0; i < json_array_get_length(devices); i++) {
            JsonObject *device = json_array_get_object_element(devices, i);
            g_autoptr(GDateTime) created = g_date_time_new_from_unix_local(
                json_object_get_int_member(device, "created"));
            g_autofree gchar *when = (created != NULL)
                ? g_date_time_format(created, "%Y-%m-%d %H:%M")
                : g_strdup("?");
            g_autofree gchar *seen = clawt_device_seen_text(
                json_object_get_int_member(device, "last_seen"),
                json_object_get_boolean_member(device, "connected"), now);

            g_print("%-11s %-22s %-17s %s%s\n",
                    json_object_get_string_member(device, "id"),
                    json_object_get_string_member(device, "name"), when,
                    seen,
                    json_object_get_boolean_member(device, "current")
                        ? "  (this connection)" : "");
        }

        if (json_object_get_int_member(payload, "idle_days") > 0)
            g_print("\nA device unseen for %" G_GINT64_FORMAT " days stops "
                    "working (daemon.device_idle_days).\n",
                    json_object_get_int_member(payload, "idle_days"));

        return EXIT_SUCCESS;
    }
}

/*
 * The sentinel: a read-only scan of the event log for recurring trouble.
 *
 * Offline, like `backup`: it reads <state_dir>/events itself and never
 * talks to the daemon, which is what lets the daemon run it as a child
 * without a scan of a week of logs ever holding the daemon's loop.  The
 * thresholds are the config's sentinel.* unless a flag says otherwise.
 */
static gint
cmd_sentinel(int argc, char *argv[])
{
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) report = NULL;
    g_autofree gchar *state_dir = NULL;
    g_autofree gchar *events = NULL;
    g_autofree gchar *output = NULL;
    ClawtSentinelOptions options;
    int arg;

    clawt_sentinel_options_init(&options);

    for (arg = 2; arg < argc; arg++) {
        const gchar *flag = argv[arg];
        const gchar *value = (arg + 1 < argc) ? argv[arg + 1] : NULL;

        if (g_strcmp0(flag, "-h") == 0 || g_strcmp0(flag, "--help") == 0) {
            g_print("Usage: clawtilla sentinel [--events DIR] [--output FILE|-]\n"
                    "                          [--days N] [--min-failures N]\n"
                    "                          [--queue-wait SECONDS]\n"
                    "                          [--daily-spend MICROS]\n"
                    "\n"
                    "Reads the event log (read-only) and groups failing tool\n"
                    "calls, long queue waits, spending and stalled turns into\n"
                    "issues, written as YAML. Defaults come from sentinel.* in\n"
                    "the config; the report goes to <state_dir>/sentinel.yaml,\n"
                    "which is what the clients' Issues view shows.\n"
                    "\n"
                    "Examples:\n"
                    "  clawtilla sentinel\n"
                    "  clawtilla sentinel --days 1 --output -\n"
                    "  clawtilla sentinel --events ~/.clawtilla/events "
                    "--min-failures 5\n");
            return EXIT_SUCCESS;
        }

        if (value == NULL) {
            g_printerr("clawtilla sentinel: %s needs a value\n", flag);
            return EXIT_FAILURE;
        }

        if (g_strcmp0(flag, "--events") == 0)
            events = g_strdup(value);
        else if (g_strcmp0(flag, "--output") == 0)
            output = g_strdup(value);
        else if (g_strcmp0(flag, "--days") == 0)
            options.days = (guint)g_ascii_strtoull(value, NULL, 10);
        else if (g_strcmp0(flag, "--min-failures") == 0)
            options.min_failures = (guint)g_ascii_strtoull(value, NULL, 10);
        else if (g_strcmp0(flag, "--queue-wait") == 0)
            options.queue_wait_seconds =
                (guint)g_ascii_strtoull(value, NULL, 10);
        else if (g_strcmp0(flag, "--daily-spend") == 0)
            options.daily_spend_micros = g_ascii_strtoll(value, NULL, 10);
        else {
            g_printerr("clawtilla sentinel: unknown option %s\n", flag);
            return EXIT_FAILURE;
        }

        arg++;
    }

    /*
     * The config supplies the defaults and the state directory.  A flag
     * given on the command line wins; a config that will not load is
     * only fatal when nothing else said where the log is.
     */
    config = clawt_config_load(opt_config_path, &error);

    if (config != NULL) {
        state_dir = clawt_config_get_path_value(config, "daemon.state_dir");

        if (!g_strv_contains((const gchar * const *)argv, "--days"))
            options.days = (guint)MAX(clawt_config_get_int(config,
                "sentinel.days"), 0);
        if (!g_strv_contains((const gchar * const *)argv, "--min-failures"))
            options.min_failures = (guint)MAX(clawt_config_get_int(config,
                "sentinel.min_failures"), 0);
        if (!g_strv_contains((const gchar * const *)argv, "--queue-wait"))
            options.queue_wait_seconds = (guint)MAX(clawt_config_get_int(
                config, "sentinel.queue_wait_seconds"), 0);
        if (!g_strv_contains((const gchar * const *)argv, "--daily-spend"))
            options.daily_spend_micros = MAX(clawt_config_get_int(config,
                "sentinel.daily_spend_micros"), 0);
    } else if (events == NULL) {
        g_printerr("clawtilla sentinel: %s\n", error->message);
        return EXIT_FAILURE;
    }

    if (events == NULL)
        events = g_build_filename(state_dir, "events", NULL);

    if (output == NULL)
        output = (state_dir != NULL)
            ? g_build_filename(state_dir, "sentinel.yaml", NULL)
            : g_strdup("-");

    report = clawt_sentinel_scan(events, &options, &error);

    if (report == NULL) {
        g_printerr("clawtilla sentinel: could not read %s: %s\n", events,
                   error->message);
        return EXIT_FAILURE;
    }

    if (g_strcmp0(output, "-") == 0) {
        g_autofree gchar *text = clawt_sentinel_report_to_yaml(report,
                                                                &error);

        if (text == NULL) {
            g_printerr("clawtilla sentinel: %s\n", error->message);
            return EXIT_FAILURE;
        }

        g_print("%s", text);
        return EXIT_SUCCESS;
    }

    if (!clawt_sentinel_write_report(report, output, events, &error)) {
        g_printerr("clawtilla sentinel: %s\n", error->message);
        return EXIT_FAILURE;
    }

    {
        JsonObject *root = json_node_get_object(report);
        JsonArray *issues = json_object_get_array_member(root, "issues");

        g_print("%u issue%s written to %s\n", json_array_get_length(issues),
                json_array_get_length(issues) == 1 ? "" : "s", output);
    }

    return EXIT_SUCCESS;
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
    { "backup", "<create|verify|preview|restore>", "Offline verified state archives", clawt_cli_backup },
    { "daemon",  "[token]",  "Run the daemon, or print its token",
      cmd_daemon },
    { "remote",  "<verb>",   "Saved connections to other machines",
      cmd_remote },
    { "device",  "<verb>",   "Pair, list and revoke client devices",
      cmd_device },
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
    { "notices", "[verb]",   "The notification list: list, read, dismiss, clear",
      cmd_notices },
    { "mute",    "<agent> [--for 2h]", "Silence an agent's notifications",
      cmd_mute },
    { "unmute",  "<agent>",  "Let an agent's notifications through again",
      cmd_mute },
    { "team",    "<verb>",   "Manage teams and who is on them",
      cmd_team },
    { "project", "<verb>",   "Projects: what work is for and may spend",
      cmd_project },
    { "history", "<verb>",   "Every version of the config, personas and skills",
      cmd_history },
    { "fleet",   "<verb>",   "Export a fleet as a pack, or import one",
      cmd_fleet },
    { "account", "<verb>",   "Separate logins for an agent CLI",
      cmd_account },
    { "policy-hook", "--agent ID --token-file PATH", "Claude Code's PreToolUse hook (run by the daemon's settings)",
      cmd_policy_hook },
    { "task",    "<verb>",   "Tasks: list, show, create, assign, comment, link, unlink, review, diff, checkout-remove, venture-retry, cancel, retry, incidents",
      cmd_task },
    { "decision", "<verb>",  "Answer what an agent has asked you",
      cmd_decision },
    { "wait",    "<verb>",   "What agents are waiting for (list, cancel)",
      cmd_wait },
    { "event",   "<verb>",   "The daemon's event history",
      cmd_event },
    { "cost",    "[--days N] [--by GROUP] [--csv]",
      "What the fleet has spent, per agent, model, origin or day",
      cmd_cost },
    { "limits",  "[--cached] [--json]",
      "What each provider login has left of its plan",
      cmd_limits },
    { "federation", "<verb>", "Link this fleet with other fleets: invites, tiers, tags",
      cmd_federation },
    { "office",  "<verb>",   "Who is where in the office, and its worlds",
      cmd_office },
    { "memory",  "<verb>",   "What an agent has written down",
      cmd_memory },
    { "sentinel", "[--output FILE|-]", "Scan the event log for recurring trouble (read-only)",
      cmd_sentinel },
    { "computer", "<verb>",  "Run commands on an agent's computer",
      cmd_computer },
    { "activity", "",        "Every turn running across the fleet",
      cmd_activity },
    { "browser", "<verb>",   "The web browsers agents drive",
      cmd_browser },
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
    { "broker",  "<verb>",   "The integration broker clawtilla runs",
      cmd_broker },
    { "routine", "<verb>",   "Standing work on a schedule",
      cmd_routine },
    { "trigger", "<verb>",   "Work started by a webhook",
      cmd_trigger },
    { "grok",    "<verb>",   "Hand work to Grok bots, and hear back",
      cmd_grok },
    { "attachment", "<verb>", "Files sent in a conversation (list, get)",
      cmd_attachment },
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

    /*
     * Take the terminal's character set from the environment.
     *
     * A program that never calls setlocale() runs in the "C" locale, and
     * g_print() / g_printerr() convert their UTF-8 to that locale's ASCII
     * when writing to a terminal -- every character outside it becomes
     * '?' (a pipe or a file gets the bytes untouched, which is why it hid).
     * The QR code from `device pair --qr` is drawn in half-block
     * characters, so it printed as a field of question marks; agent names,
     * message bodies and any other non-ASCII text were mangled the same way.
     *
     * LC_CTYPE only: LC_NUMERIC would turn "%.2f" into "0,25" under some
     * locales, and the CLI's numbers are read by scripts as well as people.
     */
    setlocale(LC_CTYPE, "");

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
        g_auto(GStrv) owned = NULL;
        g_autofree gchar **head = NULL;
        gint parsed_argc;
        gint i;

        /*
         * Two arrays: g_option_context_parse() removes what it consumed
         * by shifting the array it is handed, so the strings it drops are
         * no longer reachable from it. `owned` keeps every one to free;
         * `head` is the shallow copy the parser rearranges.
         */
        owned = g_new0(gchar *, (gsize)head_argc + 1);
        head = g_new0(gchar *, (gsize)head_argc + 1);

        for (i = 0; i < head_argc; i++) {
            owned[i] = g_strdup(argv[i]);
            head[i] = owned[i];
        }

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
            /*
             * No known verb: what the parser left is the command line
             * now, so argv has to say so. It used to keep the original,
             * and `clawtilla -s SOCK list` answered "unknown command
             * '-s'" -- the one word that was understood. The parser
             * worked on copies, so each survivor is found by identity in
             * `owned` and replaced with the original it was copied from.
             */
            g_autofree gchar **original = g_new0(gchar *, (gsize)head_argc + 1);
            gint j;

            for (i = 0; i < head_argc; i++)
                original[i] = argv[i];

            for (i = 0; i < parsed_argc; i++)
                for (j = 0; j < head_argc; j++)
                    if (head[i] == owned[j]) {
                        argv[i] = original[j];
                        break;
                    }

            argc = parsed_argc;
            argv[argc] = NULL;
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
