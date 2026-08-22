/*
 * test-workspace.c - The standard file set in an agent's workspace
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <utime.h>

#include "clawt-test-util.h"

typedef struct {
    gchar       *dir;
    ClawtConfig *config;
} Fixture;

static void
fixture_setup(Fixture *fixture, const gchar *agent_yaml)
{
    g_autofree gchar *yaml = NULL;
    g_autoptr(GError) error = NULL;

    fixture->dir = g_dir_make_tmp("clawt-ws-XXXXXX", NULL);

    yaml = g_strdup_printf(
        "daemon:\n"
        "  state_dir: \"%s/state\"\n"
        "defaults:\n"
        "  workspace_root: \"%s/agents\"\n"
        "%s",
        fixture->dir, fixture->dir, agent_yaml);

    fixture->config = clawt_config_load_from_string(yaml, &error);
    g_assert_no_error(error);
    g_assert_nonnull(fixture->config);
}

static void
fixture_teardown(Fixture *fixture)
{
    g_clear_object(&fixture->config);

    if (fixture->dir != NULL)
        clawt_test_remove_tree(fixture->dir);

    g_clear_pointer(&fixture->dir, g_free);
}

static ClawtAgentConfig *
first_agent(Fixture *fixture)
{
    GPtrArray *agents = clawt_config_get_agents(fixture->config);

    g_assert_cmpuint(agents->len, >, 0);

    return g_ptr_array_index(agents, 0);
}

static gchar *
read_workspace_file(ClawtAgentConfig *agent, const gchar *name)
{
    g_autofree gchar *path = clawt_workspace_file_path(agent, name);
    gchar *contents = NULL;

    if (path == NULL || !g_file_get_contents(path, &contents, NULL, NULL))
        return NULL;

    return contents;
}

/*
 * Every file in the set is written, and every identity file is one the
 * loader names.  The two lists drifting is the failure this catches: a
 * file added to the set but not the loader is scaffolded and never read,
 * which looks exactly like the agent ignoring it.
 */
static void
test_scaffold_writes_the_standard_set(void)
{
    Fixture fixture = { 0 };
    ClawtAgentConfig *agent;
    const ClawtWorkspaceFile *files;
    g_autoptr(GError) error = NULL;
    g_auto(GStrv) identity = NULL;
    g_autofree gchar *loader = NULL;
    guint n_files = 0;
    guint i;

    fixture_setup(&fixture, "agents:\n  - id: scribe\n");
    agent = first_agent(&fixture);

    g_assert_true(clawt_workspace_scaffold(agent, &error));
    g_assert_no_error(error);

    files = clawt_workspace_files(&n_files);
    g_assert_cmpuint(n_files, >, 0);

    for (i = 0; i < n_files; i++) {
        g_autofree gchar *path =
            clawt_workspace_file_path(agent, files[i].name);

        g_assert_nonnull(path);

        /*
         * A generated file has no template and is not scaffolded --
         * .mcp.json is written by clawt_workspace_write_mcp_config(),
         * which needs a socket and a state directory this has neither
         * of.
         */
        if (files[i].generated) {
            g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
            continue;
        }

        g_assert_true(g_file_test(path, G_FILE_TEST_EXISTS));
    }

    /* CLAUDE.md is one line and nothing else: it points at AGENTS.md. */
    {
        g_autofree gchar *claude = read_workspace_file(agent, "CLAUDE.md");

        g_assert_nonnull(claude);
        g_assert_cmpstr(g_strstrip(claude), ==, "@AGENTS.md");
    }

    /* AGENTS.md names every identity file, in order. */
    loader = read_workspace_file(agent, "AGENTS.md");
    g_assert_nonnull(loader);

    identity = clawt_workspace_identity_files();
    g_assert_nonnull(identity[0]);

    {
        const gchar *cursor = loader;

        for (i = 0; identity[i] != NULL; i++) {
            g_autofree gchar *include = g_strdup_printf("@%s", identity[i]);
            const gchar *found = strstr(cursor, include);

            g_assert_nonnull(found);
            cursor = found;
        }
    }

    fixture_teardown(&fixture);
}

/*
 * The defaults are a starting point, so a second start must not restore
 * them over the user's edits -- which is the entire reason the files are
 * editable in the first place.
 */
static void
test_scaffold_never_overwrites(void)
{
    Fixture fixture = { 0 };
    ClawtAgentConfig *agent;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *path = NULL;
    g_autofree gchar *after = NULL;

    fixture_setup(&fixture, "agents:\n  - id: scribe\n");
    agent = first_agent(&fixture);

    g_assert_true(clawt_workspace_scaffold(agent, &error));

    path = clawt_workspace_file_path(agent, "SOUL.org");
    g_assert_true(g_file_set_contents(path, "MINE\n", -1, NULL));

    g_assert_true(clawt_workspace_scaffold(agent, &error));
    g_assert_no_error(error);

    g_assert_true(g_file_get_contents(path, &after, NULL, NULL));
    g_assert_cmpstr(after, ==, "MINE\n");

    fixture_teardown(&fixture);
}

/*
 * The templates are filled in from the agent's own configuration.  A
 * workspace full of "your agent id here" is one nobody edits, and an
 * agent that has to be told what computer it has wastes its first turns
 * finding out.
 */
static void
test_scaffold_describes_this_agent(void)
{
    Fixture fixture = { 0 };
    ClawtAgentConfig *agent;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *identity = NULL;
    g_autofree gchar *tools = NULL;

    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: scribe\n"
                  "    name: \"The Scribe\"\n"
                  "    description: \"Writes things down.\"\n"
                  "    computer: {type: container}\n");
    agent = first_agent(&fixture);

    g_assert_true(clawt_workspace_scaffold(agent, &error));
    g_assert_no_error(error);

    identity = read_workspace_file(agent, "IDENTITY.org");
    g_assert_nonnull(identity);
    g_assert_nonnull(strstr(identity, "scribe"));
    g_assert_nonnull(strstr(identity, "The Scribe"));
    g_assert_nonnull(strstr(identity, "Writes things down."));

    tools = read_workspace_file(agent, "TOOLS.org");
    g_assert_nonnull(tools);

    /* The orchestration tools it actually has, named. */
    g_assert_nonnull(strstr(tools, "clawtilla_ask_agent"));
    g_assert_nonnull(strstr(tools, "clawtilla_delegate"));
    g_assert_nonnull(strstr(tools, "clawtilla_mailbox_ack"));

    /* And its own computer, described rather than left to discovery. */
    g_assert_nonnull(strstr(tools, "container of your own"));

    /* No template marker survives into a scaffolded file. */
    g_assert_null(strstr(tools, "{{"));
    g_assert_null(strstr(identity, "{{"));

    fixture_teardown(&fixture);
}

/*
 * An agent with no computer has to be told so.  It used to be left to
 * find out by running a command and being refused, which costs a turn
 * and reads like a broken tool.
 */
static void
test_scaffold_says_when_there_is_no_computer(void)
{
    Fixture fixture = { 0 };
    ClawtAgentConfig *agent;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *tools = NULL;

    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: talker\n"
                  "    computer: {type: none}\n");
    agent = first_agent(&fixture);

    g_assert_true(clawt_workspace_scaffold(agent, &error));

    tools = read_workspace_file(agent, "TOOLS.org");
    g_assert_nonnull(tools);
    g_assert_nonnull(strstr(tools, "no computer"));

    fixture_teardown(&fixture);
}

/*
 * The file name in an `agent.files` reply comes from a client, so a name
 * that climbs out of the workspace has to be refused rather than
 * resolved -- "../../agents/other/credentials" is a perfectly ordinary
 * relative path.
 */
static void
test_file_path_refuses_escaping(void)
{
    Fixture fixture = { 0 };
    ClawtAgentConfig *agent;
    static const gchar *const hostile[] = {
        "../SOUL.org",
        "../../etc/passwd",
        "sub/SOUL.org",
        "/etc/passwd",
        "..",
        ".",
        "",
        NULL
    };
    g_autofree gchar *ok = NULL;
    gsize i;

    fixture_setup(&fixture, "agents:\n  - id: scribe\n");
    agent = first_agent(&fixture);

    for (i = 0; hostile[i] != NULL; i++)
        g_assert_null(clawt_workspace_file_path(agent, hostile[i]));

    /* A plain name still resolves, inside the workspace. */
    ok = clawt_workspace_file_path(agent, "SOUL.org");
    g_assert_nonnull(ok);
    g_assert_true(g_str_has_suffix(ok, "/scribe/SOUL.org"));

    fixture_teardown(&fixture);
}

/*
 * Scaffolded files that nothing loads are decoration.  The rendered
 * libreclaw config has to name them when the user has not chosen a list
 * of their own.
 */
static void
test_rendered_config_loads_the_set(void)
{
    Fixture fixture = { 0 };
    ClawtAgentConfig *agent;
    g_auto(GStrv) identity = NULL;
    g_autofree gchar *rendered = NULL;
    gsize i;

    fixture_setup(&fixture, "agents:\n  - id: scribe\n");
    agent = first_agent(&fixture);

    rendered = clawt_config_render_agent(fixture.config, agent,
                                         "/tmp/agents.sock", fixture.dir,
                                         NULL);
    g_assert_nonnull(rendered);

    identity = clawt_workspace_identity_files();

    for (i = 0; identity[i] != NULL; i++)
        g_assert_nonnull(strstr(rendered, identity[i]));

    fixture_teardown(&fixture);
}

/*
 * A configured list wins.  Defaulting is a convenience, not a policy,
 * and an agent whose persona is deliberately one file should not have
 * six more appended to it.
 */
static void
test_configured_identity_files_win(void)
{
    Fixture fixture = { 0 };
    ClawtAgentConfig *agent;
    g_autofree gchar *rendered = NULL;

    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: scribe\n"
                  "    persona:\n"
                  "      identity_files: [\"ONLY.org\"]\n");
    agent = first_agent(&fixture);

    rendered = clawt_config_render_agent(fixture.config, agent,
                                         "/tmp/agents.sock", fixture.dir,
                                         NULL);
    g_assert_nonnull(rendered);
    g_assert_nonnull(strstr(rendered, "ONLY.org"));
    g_assert_null(strstr(rendered, "SOUL.org"));

    fixture_teardown(&fixture);
}

/*
 * The .mcp.json is what puts clawtilla's tools into the agent's session.
 *
 * An agent runs an AI CLI, and the only way such a CLI can be given
 * tools is a config naming an MCP server. Without this file the agent
 * has a mailbox, peers and a container it cannot reach -- and will tell
 * you so if you ask it.
 */
static void
test_mcp_config_is_written(void)
{
    Fixture fixture = { 0 };
    ClawtAgentConfig *agent;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *path = NULL;
    g_autofree gchar *text = NULL;

    fixture_setup(&fixture, "agents:\n  - id: scribe\n");
    agent = first_agent(&fixture);

    g_assert_true(clawt_workspace_scaffold(agent, &error));
    g_assert_true(clawt_workspace_write_mcp_config(
        agent, "/run/clawtilla.sock", "/state/scribe", &error));
    g_assert_no_error(error);

    path = clawt_workspace_file_path(agent, ".mcp.json");
    g_assert_nonnull(path);
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));

    /* Named so the CLI finds it, and carrying who to act as. */
    g_assert_nonnull(strstr(text, "mcpServers"));
    g_assert_nonnull(strstr(text, "clawtilla-mcp-server"));
    g_assert_nonnull(strstr(text, "--agent"));
    g_assert_nonnull(strstr(text, "scribe"));
    g_assert_nonnull(strstr(text, "/run/clawtilla.sock"));

    /*
     * The token path, not the token. A workspace is mounted into
     * containers and read by people; the secret stays in the state
     * directory.
     */
    g_assert_nonnull(strstr(text, "/state/scribe/token"));

    fixture_teardown(&fixture);
}

/*
 * Rewritten every start, unlike the org files: it is generated rather
 * than authored, and a stale one points at a socket that has moved.
 */
static void
test_mcp_config_is_regenerated(void)
{
    Fixture fixture = { 0 };
    ClawtAgentConfig *agent;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *path = NULL;
    g_autofree gchar *text = NULL;

    fixture_setup(&fixture, "agents:\n  - id: scribe\n");
    agent = first_agent(&fixture);

    g_assert_true(clawt_workspace_scaffold(agent, &error));
    g_assert_true(clawt_workspace_write_mcp_config(agent, "/old.sock",
                                                    "/state/scribe", &error));
    g_assert_true(clawt_workspace_write_mcp_config(agent, "/new.sock",
                                                    "/state/scribe", &error));

    path = clawt_workspace_file_path(agent, ".mcp.json");
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
    g_assert_nonnull(strstr(text, "/new.sock"));
    g_assert_null(strstr(text, "/old.sock"));

    fixture_teardown(&fixture);
}


/*
 * A server the user added by hand survives a restart.
 *
 * This file is how an agent is given MCP servers, so it is a file
 * people edit. It used to be regenerated wholesale on every start,
 * which meant anything added to it lasted exactly until the agent was
 * next restarted -- and the loss was silent.
 */
static void
test_mcp_config_keeps_what_the_user_added(void)
{
    Fixture fixture = { 0 };
    ClawtAgentConfig *agent;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autofree gchar *path = NULL;
    g_autofree gchar *text = NULL;
    JsonObject *servers;
    JsonObject *root;

    fixture_setup(&fixture, "agents:\n  - id: scribe\n");
    agent = first_agent(&fixture);

    g_assert_true(clawt_workspace_scaffold(agent, &error));
    g_assert_true(clawt_workspace_write_mcp_config(agent, "/old.sock",
                                                    "/state/scribe", &error));
    g_assert_no_error(error);

    path = clawt_workspace_file_path(agent, ".mcp.json");

    /* What editing the file in $EDITOR amounts to. */
    g_assert_true(g_file_set_contents(path,
        "{\n"
        "  \"mcpServers\": {\n"
        "    \"clawtilla\": { \"command\": \"stale\" },\n"
        "    \"gowl\": { \"command\": \"gowl-mcp\", \"args\": [\"--tty\"] }\n"
        "  },\n"
        "  \"somethingElse\": true\n"
        "}\n", -1, NULL));

    g_assert_true(clawt_workspace_write_mcp_config(agent, "/new.sock",
                                                    "/state/scribe", &error));
    g_assert_no_error(error);

    g_assert_true(json_parser_load_from_file(parser, path, &error));
    g_assert_no_error(error);

    root = json_node_get_object(json_parser_get_root(parser));
    servers = json_object_get_object_member(root, "mcpServers");

    /* Theirs, untouched, arguments and all. */
    g_assert_true(json_object_has_member(servers, "gowl"));
    g_assert_cmpstr(json_object_get_string_member(
                        json_object_get_object_member(servers, "gowl"),
                        "command"), ==, "gowl-mcp");
    g_assert_cmpuint(json_array_get_length(
                         json_object_get_array_member(
                             json_object_get_object_member(servers, "gowl"),
                             "args")), ==, 1);

    /* A top-level key clawtilla has never heard of survives too. */
    g_assert_true(json_object_get_boolean_member(root, "somethingElse"));

    /* Ours, current: the socket moved and the entry followed. */
    g_assert_true(json_object_has_member(servers, "clawtilla"));
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
    g_assert_nonnull(strstr(text, "/new.sock"));
    g_assert_null(strstr(text, "stale"));

    fixture_teardown(&fixture);
}

/*
 * A file that does not parse is moved aside, not overwritten.
 *
 * A stray comma is not a reason to delete something a person wrote
 * without leaving them a copy of it.
 */
static void
test_mcp_config_keeps_a_broken_file(void)
{
    Fixture fixture = { 0 };
    ClawtAgentConfig *agent;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *path = NULL;
    g_autofree gchar *aside = NULL;
    g_autofree gchar *text = NULL;

    fixture_setup(&fixture, "agents:\n  - id: scribe\n");
    agent = first_agent(&fixture);

    g_assert_true(clawt_workspace_scaffold(agent, &error));

    path = clawt_workspace_file_path(agent, ".mcp.json");
    g_assert_true(g_file_set_contents(path, "{ not json,", -1, NULL));

    g_test_expect_message("Clawtilla", G_LOG_LEVEL_WARNING, "*not valid JSON*");
    g_assert_true(clawt_workspace_write_mcp_config(agent, "/run/c.sock",
                                                    "/state/scribe", &error));
    g_test_assert_expected_messages();
    g_assert_no_error(error);

    aside = g_strconcat(path, ".bad", NULL);
    g_assert_true(g_file_get_contents(aside, &text, NULL, NULL));
    g_assert_cmpstr(text, ==, "{ not json,");

    g_clear_pointer(&text, g_free);
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
    g_assert_nonnull(strstr(text, "clawtilla"));

    fixture_teardown(&fixture);
}

/*
 * Nothing changed means nothing written.
 *
 * An editor with the file open reloads it on every daemon start
 * otherwise, and a workspace is a directory people keep in git.
 */
static void
test_mcp_config_leaves_an_unchanged_file_alone(void)
{
    Fixture fixture = { 0 };
    ClawtAgentConfig *agent;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *path = NULL;
    struct utimbuf backdated;
    GStatBuf before;
    GStatBuf after;

    fixture_setup(&fixture, "agents:\n  - id: scribe\n");
    agent = first_agent(&fixture);

    g_assert_true(clawt_workspace_scaffold(agent, &error));
    g_assert_true(clawt_workspace_write_mcp_config(agent, "/run/c.sock",
                                                    "/state/scribe", &error));

    path = clawt_workspace_file_path(agent, ".mcp.json");

    /* Backdated, so "unchanged" is a fact rather than a timer race. */
    backdated.actime = 1000000;
    backdated.modtime = 1000000;
    g_assert_cmpint(g_utime(path, &backdated), ==, 0);
    g_assert_cmpint(g_stat(path, &before), ==, 0);

    g_assert_true(clawt_workspace_write_mcp_config(agent, "/run/c.sock",
                                                    "/state/scribe", &error));
    g_assert_no_error(error);

    g_assert_cmpint(g_stat(path, &after), ==, 0);
    g_assert_cmpint(before.st_mtime, ==, after.st_mtime);

    fixture_teardown(&fixture);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/workspace/scaffold-writes-the-set",
                    test_scaffold_writes_the_standard_set);
    g_test_add_func("/workspace/scaffold-never-overwrites",
                    test_scaffold_never_overwrites);
    g_test_add_func("/workspace/describes-this-agent",
                    test_scaffold_describes_this_agent);
    g_test_add_func("/workspace/no-computer-is-said-out-loud",
                    test_scaffold_says_when_there_is_no_computer);
    g_test_add_func("/workspace/mcp-config", test_mcp_config_is_written);
    g_test_add_func("/workspace/mcp-config-regenerated",
                    test_mcp_config_is_regenerated);
    g_test_add_func("/workspace/mcp-config-keeps-your-servers",
                    test_mcp_config_keeps_what_the_user_added);
    g_test_add_func("/workspace/mcp-config-keeps-a-broken-file",
                    test_mcp_config_keeps_a_broken_file);
    g_test_add_func("/workspace/mcp-config-leaves-unchanged-alone",
                    test_mcp_config_leaves_an_unchanged_file_alone);
    g_test_add_func("/workspace/file-path-refuses-escaping",
                    test_file_path_refuses_escaping);
    g_test_add_func("/workspace/rendered-config-loads-the-set",
                    test_rendered_config_loads_the_set);
    g_test_add_func("/workspace/configured-files-win",
                    test_configured_identity_files_win);

    return g_test_run();
}
