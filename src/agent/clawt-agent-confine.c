/*
 * clawt-agent-confine.c - Putting an agent's own CLI in a sandbox
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "agent/clawt-agent-confine.h"

#include <stdio.h>
#include <string.h>
#include <libreclaw.h>
#include <ai-glib.h>

gboolean
clawt_agent_confine_wanted(ClawtAgentConfig *config)
{
    g_return_val_if_fail(config != NULL, FALSE);

    return clawt_agent_config_get_enum(config, "runtime.confine") ==
           CLAWT_RUNTIME_CONFINE_BWRAP;
}

gboolean
clawt_agent_confine_available(void)
{
    g_autofree gchar *bwrap = g_find_program_in_path("bwrap");

    return bwrap != NULL;
}

gboolean
clawt_agent_confine_check(ClawtAgentConfig *config, GError **error)
{
    g_return_val_if_fail(config != NULL, FALSE);

    if (!clawt_agent_confine_wanted(config))
        return TRUE;

    /*
     * An embedded agent is the daemon: there is no child process to wrap,
     * and running it anyway would be running it unconfined while its
     * configuration says otherwise.  Which key asked for it is named,
     * because an agent that inherited bwrap from defaults.confine has
     * nothing in its own block to explain the refusal.
     */
    if (clawt_agent_config_get_enum(config, "runtime.type") ==
            CLAWT_RUNTIME_EMBEDDED) {
        gboolean own = clawt_agent_config_has_key(config, "runtime.confine");

        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT,
                    "runtime.confine: bwrap%s cannot apply to "
                    "runtime.type: embedded -- an embedded agent runs "
                    "inside the daemon, so there is no process of its own "
                    "to put in a sandbox. Use runtime.type: process, or "
                    "set this agent's runtime.confine: none",
                    own ? "" : " (from defaults.confine)");
        return FALSE;
    }

    if (!clawt_agent_confine_available()) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT,
                            "runtime.confine: bwrap needs bubblewrap, which "
                            "is not installed where the daemon runs; "
                            "install it (Fedora: bubblewrap) or set "
                            "runtime.confine: none. The agent is not run "
                            "unconfined instead");
        return FALSE;
    }

    return TRUE;
}

/* ── Where a CLI keeps its login ─────────────────────────────────── */

GStrv
clawt_agent_confine_default_login_paths(const gchar *provider)
{
    g_autoptr(GPtrArray) out = g_ptr_array_new_with_free_func(g_free);
    const gchar *home = g_get_home_dir();
    const gchar *cli;

    /*
     * Normalised the way libreclaw does, because that decides which CLI
     * runs: "claude" and "claude-code" are one login, and an unknown name
     * runs Claude Code.
     */
    cli = lc_provider_type_normalize(provider, "agent");

    if (g_strcmp0(cli, "claude-code") == 0 ||
        g_strcmp0(cli, "claude-tmux") == 0) {
        /*
         * Two places: the directory, and a JSON file beside it that holds
         * the account state and is rewritten on every token refresh.
         */
        g_ptr_array_add(out, g_build_filename(home, ".claude", NULL));
        g_ptr_array_add(out, g_build_filename(home, ".claude.json", NULL));
    } else if (g_strcmp0(cli, "codex-cli") == 0) {
        g_ptr_array_add(out, g_build_filename(home, ".codex", NULL));
    } else if (g_strcmp0(cli, "grok-build") == 0) {
        g_ptr_array_add(out, g_build_filename(home, ".grok", NULL));
    } else if (g_strcmp0(cli, "opencode") == 0) {
        g_ptr_array_add(out, g_build_filename(g_get_user_config_dir(),
                                              "opencode", NULL));
        g_ptr_array_add(out, g_build_filename(g_get_user_data_dir(),
                                              "opencode", NULL));
        g_ptr_array_add(out, g_build_filename(g_get_user_state_dir(),
                                              "opencode", NULL));
    } else if (g_strcmp0(cli, "cursor") == 0) {
        g_ptr_array_add(out, g_build_filename(home, ".cursor", NULL));
        g_ptr_array_add(out, g_build_filename(g_get_user_config_dir(),
                                              "cursor", NULL));
    } else if (g_strcmp0(cli, "antigravity") == 0) {
        g_ptr_array_add(out, g_build_filename(home, ".gemini", NULL));
    }

    g_ptr_array_add(out, NULL);

    return (GStrv)g_ptr_array_free(g_steal_pointer(&out), FALSE);
}

/* ── Programs and what they need ─────────────────────────────────── */

/*
 * Binds a path when it exists and the sandbox does not already reach it.
 *
 * Asked of the sandbox's own predicate, so a program inside the workspace
 * is not bound a second time read-only over the writable bind -- which
 * would make the agent's own files unwritable for no reason anybody could
 * see from the configuration.
 */
static void
grant(ClawtSandbox *sandbox, const gchar *path, gboolean writable)
{
    g_autofree gchar *real = NULL;

    if (path == NULL || *path == '\0' || !g_file_test(path, G_FILE_TEST_EXISTS))
        return;

    if (clawt_sandbox_path_is_allowed(sandbox, path))
        return;

    /*
     * /usr and /etc are in every box already, read-only; binding a
     * program from there again says nothing and makes the argv harder to
     * read.  Asked of the resolved path, since /bin/sh is /usr/bin/bash.
     */
    real = clawt_canonicalize_missing(path);
    if (!writable && real != NULL &&
        (clawt_path_is_within(real, "/usr") ||
         clawt_path_is_within(real, "/etc")))
        return;

    clawt_sandbox_add_mount_path(sandbox, path, writable);
}

/*
 * The directory a program's package lives in, when it has one.
 *
 * An npm-installed CLI is a script in node_modules/<pkg>/bin that
 * requires the rest of <pkg>: binding only the script gives a box in which
 * the CLI exists and dies on its first require().  Read off the path --
 * the component after the last node_modules, two for an @scope -- rather
 * than by walking up looking for package.json, which would find one in a
 * home directory and bind all of it.  NULL for anything else, which is
 * the common case of a single native binary.
 */
static gchar *
package_root(const gchar *real)
{
    const gchar *marker = "/node_modules/";
    const gchar *found = NULL;
    const gchar *p = real;
    const gchar *name;
    const gchar *end;

    while ((p = strstr(p, marker)) != NULL) {
        found = p;
        p += strlen(marker);
    }

    if (found == NULL)
        return NULL;

    name = found + strlen(marker);
    end = strchr(name, '/');

    if (end != NULL && name[0] == '@')
        end = strchr(end + 1, '/');

    if (end == NULL)
        return NULL;

    return g_strndup(real, (gsize)(end - real));
}

/*
 * The interpreter a script names on its #! line, resolved, or NULL.
 *
 * `#!/usr/bin/env node` is how every npm CLI starts, and env finds node on
 * PATH -- which, for a node from nvm or Homebrew, is a directory under
 * $HOME the box would otherwise not contain.
 */
static gchar *
script_interpreter(const gchar *path)
{
    gchar head[256];
    gsize length;
    FILE *file;
    g_auto(GStrv) words = NULL;
    gchar *newline;
    const gchar *program;

    /*
     * Only the first line, read as bytes.  The thing being looked at is
     * as often a 200 MB native CLI as a script, and reading the whole of
     * it on every agent start to learn it has no #! is a cost with
     * nothing to show for it.
     */
    file = fopen(path, "rb");
    if (file == NULL)
        return NULL;

    length = fread(head, 1, sizeof(head) - 1, file);
    fclose(file);
    head[length] = '\0';

    if (length < 3 || head[0] != '#' || head[1] != '!')
        return NULL;

    newline = strchr(head, '\n');
    if (newline != NULL)
        *newline = '\0';

    words = g_strsplit_set(head + 2, " \t", -1);
    if (words == NULL || words[0] == NULL)
        return NULL;

    program = g_strstrip(words[0]);

    if (g_str_has_suffix(program, "/env")) {
        guint i;

        for (i = 1; words[i] != NULL; i++) {
            if (*words[i] == '\0' || words[i][0] == '-')
                continue;

            return g_find_program_in_path(words[i]);
        }

        return NULL;
    }

    return g_strdup(program);
}

/*
 * Makes a program runnable inside the box: the name it is called by, the
 * file that name resolves to, its package if it is one, and its
 * interpreter if it is a script.  System paths are already there and add
 * nothing.
 */
static void
grant_program(ClawtSandbox *sandbox, const gchar *program, guint depth)
{
    g_autofree gchar *found = NULL;
    g_autofree gchar *real = NULL;
    g_autofree gchar *package = NULL;
    g_autofree gchar *interpreter = NULL;

    if (program == NULL || *program == '\0' || depth > 2)
        return;

    found = g_path_is_absolute(program) ? g_strdup(program)
                                        : g_find_program_in_path(program);

    if (found == NULL)
        return;

    real = clawt_canonicalize_missing(found);

    /*
     * The name as the CLI will be called by, so PATH lookups inside the
     * box find it -- bwrap follows a symlinked source, so this puts the
     * real file at the link's own spelling -- and the real file at its own
     * path, so a program that finds its siblings through /proc/self/exe
     * finds them.
     */
    grant(sandbox, found, FALSE);
    grant(sandbox, real, FALSE);

    /*
     * A bind is of the canonical path, so a CLI installed as a symlink
     * on PATH -- ~/.local/bin/claude into a versions directory -- had
     * its target in the box and its name missing: libreclaw found it
     * outside and the exec inside said "No such file or directory".
     */
    clawt_sandbox_add_symlink(sandbox, found);

    if (real != NULL) {
        package = package_root(real);
        grant(sandbox, package, FALSE);
    }

    interpreter = script_interpreter(real != NULL ? real : found);
    if (interpreter != NULL)
        grant_program(sandbox, interpreter, depth + 1);
}

/*
 * Every program and file the workspace's .mcp.json hands the CLI.
 *
 * The CLI starts these itself, inside the box, so a server whose command
 * lives in a build tree -- clawtilla-mcp-server, beside the daemon -- is
 * missing there unless it is bound.  Arguments that are existing absolute
 * paths are bound too, read-only: a token file, a config file.  Anything
 * the sandbox already reaches is left alone.
 */
static void
grant_mcp_targets(ClawtSandbox *sandbox, const gchar *workspace)
{
    g_autofree gchar *path = g_build_filename(workspace, ".mcp.json", NULL);
    g_autoptr(JsonParser) parser = json_parser_new();
    JsonNode *root;
    JsonObject *servers;
    g_autoptr(GList) members = NULL;
    GList *l;

    if (!json_parser_load_from_file(parser, path, NULL))
        return;

    root = json_parser_get_root(parser);

    if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root) ||
        json_node_get_object(root) == NULL)
        return;

    if (!json_object_has_member(json_node_get_object(root), "mcpServers"))
        return;

    if (!JSON_NODE_HOLDS_OBJECT(json_object_get_member(
            json_node_get_object(root), "mcpServers")))
        return;

    servers = json_object_get_object_member(json_node_get_object(root),
                                            "mcpServers");

    if (servers == NULL)
        return;

    members = json_object_get_members(servers);

    for (l = members; l != NULL; l = l->next) {
        JsonNode *node = json_object_get_member(servers, l->data);
        JsonObject *server;

        if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node))
            continue;

        server = json_node_get_object(node);

        if (server == NULL)
            continue;

        if (json_object_has_member(server, "command") &&
            JSON_NODE_HOLDS_VALUE(json_object_get_member(server, "command")))
            grant_program(sandbox,
                          json_object_get_string_member(server, "command"),
                          0);

        if (json_object_has_member(server, "args") &&
            JSON_NODE_HOLDS_ARRAY(json_object_get_member(server, "args"))) {
            JsonArray *args = json_object_get_array_member(server, "args");
            guint i;

            for (i = 0; args != NULL && i < json_array_get_length(args);
                 i++) {
                JsonNode *arg = json_array_get_element(args, i);
                const gchar *text;

                if (arg == NULL || !JSON_NODE_HOLDS_VALUE(arg) ||
                    json_node_get_value_type(arg) != G_TYPE_STRING)
                    continue;

                text = json_node_get_string(arg);

                if (text != NULL && g_path_is_absolute(text))
                    grant(sandbox, text, FALSE);
            }
        }
    }
}

/* ── The box ─────────────────────────────────────────────────────── */

ClawtSandbox *
clawt_agent_confine_build(ClawtAgentConfig     *config,
                          const gchar          *state_dir,
                          GPtrArray            *mounts,
                          const gchar * const  *sockets,
                          const gchar          *libreclaw,
                          GError              **error)
{
    g_autoptr(ClawtSandbox) sandbox = NULL;
    g_autoptr(GPtrArray) own_mounts = NULL;
    g_autofree gchar *workspace = NULL;
    g_auto(GStrv) extra = NULL;
    guint i;

    g_return_val_if_fail(config != NULL, NULL);

    workspace = clawt_agent_config_get_workspace(config);

    if (workspace == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT,
                            "runtime.confine: bwrap needs a workspace to "
                            "put the agent in, and this one has none");
        return NULL;
    }

    sandbox = clawt_sandbox_new(CLAWT_CONFINE_BWRAP, workspace);

    /*
     * The network, from runtime.network, decided here with the binds so
     * the box and the setting cannot disagree.  Open shares the
     * machine's; gated and none get a namespace of their own with only
     * loopback in it, and the process runtime adds the relay that makes
     * the egress gate and the broker reachable from there.
     */
    clawt_sandbox_set_allow_network(
        sandbox, clawt_agent_config_get_enum(config, "runtime.network") ==
                 CLAWT_RUNTIME_NETWORK_OPEN);
    clawt_sandbox_set_private_home(sandbox, g_get_home_dir());
    clawt_sandbox_set_agent_confined(sandbox, TRUE);

    /*
     * The state directory: the rendered config.yaml libreclaw is started
     * with, the session store, the usage database, the link token and the
     * credential files it reads.  Usually the workspace itself, in which
     * case it is already in.
     */
    grant(sandbox, state_dir, TRUE);

    /*
     * The mounts the agent is told about, at their host spelling --
     * because its own tools run here and read the host path, which is
     * what clawt_computer_describe_mounts() tells it.  In the mode they
     * were declared in: a mount bound wider than it was written is the
     * direction that costs something.
     */
    if (mounts == NULL) {
        own_mounts = clawt_agent_config_get_mounts(config);
        mounts = own_mounts;
    }

    for (i = 0; mounts != NULL && i < mounts->len; i++) {
        ClawtMount *mount = g_ptr_array_index(mounts, i);

        grant(sandbox, clawt_mount_get_source(mount),
              clawt_mount_get_mode(mount) == CLAWT_MOUNT_MODE_RW);
    }

    /*
     * The CLI's login, writable: an OAuth token is refreshed in place, and
     * a CLI that cannot write it back signs out an hour into its first
     * session.  An `accounts:` entry names its directory; otherwise the
     * CLI's own default.
     */
    {
        ClawtConfig *fleet = clawt_agent_config_get_config(config);
        const gchar *account_id =
            clawt_agent_config_get_string(config, "model.account");
        g_autoptr(ClawtAccountSpec) account = NULL;

        if (fleet != NULL && account_id != NULL && *account_id != '\0')
            account = clawt_config_get_account(fleet, account_id);

        if (account != NULL && account->config_dir != NULL) {
            grant(sandbox, account->config_dir, TRUE);
        } else {
            g_auto(GStrv) logins = clawt_agent_confine_default_login_paths(
                clawt_agent_config_get_string(config, "model.provider"));

            for (i = 0; logins[i] != NULL; i++)
                grant(sandbox, logins[i], TRUE);
        }
    }

    /*
     * The AI CLI itself, found the way ai-glib will find it inside: by
     * asking the client class, which knows its binary's name and the
     * variable that overrides it.
     */
    {
        const gchar *provider =
            clawt_agent_config_get_string(config, "model.provider");
        const gchar *cli = lc_provider_type_normalize(provider, "agent");
        g_autoptr(AiCliClient) probe =
            lc_client_factory_new(cli, NULL, NULL, NULL, FALSE);

        if (probe != NULL) {
            g_autofree gchar *executable =
                ai_cli_client_resolve_executable(probe, NULL);

            grant_program(sandbox, executable, 0);
        }

        /*
         * claude-tmux drives the CLI through tmux, which has to be in the
         * box too; it is nearly always in /usr and costs nothing then.
         */
        if (g_strcmp0(cli, "claude-tmux") == 0)
            grant_program(sandbox, "tmux", 0);
    }

    grant_program(sandbox, libreclaw, 0);
    grant_mcp_targets(sandbox, workspace);

    /*
     * The daemon's sockets, writable because connect() on a unix socket is
     * a write.  Each socket rather than the directory holding it: that
     * directory holds every agent's link and the operator's IPC socket.
     */
    for (i = 0; sockets != NULL && sockets[i] != NULL; i++)
        grant(sandbox, sockets[i], TRUE);

    /* What the operator named, read-only. */
    extra = clawt_agent_config_get_string_list(config,
                                               "runtime.confine_paths");

    for (i = 0; extra != NULL && extra[i] != NULL; i++) {
        g_autofree gchar *expanded = clawt_expand_path(extra[i]);

        if (!g_file_test(expanded, G_FILE_TEST_EXISTS)) {
            g_warning("agent %s: runtime.confine_paths names %s, which "
                      "does not exist; it is left out of the sandbox",
                      clawt_agent_config_get_id(config), expanded);
            continue;
        }

        grant(sandbox, expanded, FALSE);
    }

    /*
     * On a host computer the operator has already said what is
     * off-limits, and the agent is told so; those are denied in this box
     * as well, so the sentence is true of its own tools too.
     */
    if (clawt_agent_config_get_enum(config, "computer.type") ==
            CLAWT_COMPUTER_HOST) {
        g_auto(GStrv) deny = clawt_agent_config_get_string_list(
            config, "computer.host.deny_paths");

        for (i = 0; deny != NULL && deny[i] != NULL; i++) {
            g_autofree gchar *expanded = clawt_expand_path(deny[i]);

            clawt_sandbox_add_deny_path(sandbox, expanded);
        }
    }

    return g_steal_pointer(&sandbox);
}

/* ── What the clients say ────────────────────────────────────────── */

const gchar *
clawt_agent_confine_label(void)
{
    return "Sandbox its own tools (bubblewrap)";
}

const gchar *
clawt_agent_confine_explanation(void)
{
    return "Runs the agent's CLI -- its own Bash, Read and Write, and its "
           "MCP servers -- inside bubblewrap, which can reach only its "
           "workspace, its mounts and its login. Takes effect at its next "
           "start. Without bubblewrap installed the agent is disabled "
           "(shadow), never run unconfined.";
}

const gchar *
clawt_agent_confine_notice(const gchar *mode,
                           const gchar *runtime_type,
                           gboolean     available)
{
    if (g_strcmp0(mode, "bwrap") != 0)
        return NULL;

    if (g_strcmp0(runtime_type, "embedded") == 0)
        return "An embedded agent runs inside the daemon and cannot be "
               "sandboxed, so it is disabled (shadow) until its runtime is "
               "process or this is turned off.";

    if (!available)
        return "bubblewrap is not installed where the daemon runs, so "
               "this agent is disabled (shadow) until it is. Install "
               "bubblewrap (Fedora: bubblewrap) or turn this off.";

    return NULL;
}
