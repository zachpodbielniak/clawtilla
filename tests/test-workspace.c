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
 * The scaffolded files must not promise a tool does something it does
 * not do.
 *
 * clawtilla_ask_agent is dispatched to the same handler as
 * clawtilla_message_agent -- see the shared branch in
 * clawt_mcp_tools_call() -- so it queues the message, answers "Queued
 * for ...", and returns.  Nothing blocks, nothing waits, and there is no
 * timeout to hit.  The tool's own description says so; three lines in
 * these templates said the opposite, and an agent that read both
 * believed the wrong one and sat waiting for a reply that was never
 * coming back through that call.
 */
static void
test_scaffold_does_not_promise_ask_agent_blocks(void)
{
    Fixture fixture = { 0 };
    ClawtAgentConfig *agent;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *agents_org = NULL;
    g_autofree gchar *tools = NULL;
    g_autofree gchar *gotchas = NULL;

    fixture_setup(&fixture, "agents:\n  - id: scribe\n");
    agent = first_agent(&fixture);

    g_assert_true(clawt_workspace_scaffold(agent, &error));
    g_assert_no_error(error);

    agents_org = read_workspace_file(agent, "AGENTS.org");
    tools = read_workspace_file(agent, "TOOLS.org");
    gotchas = read_workspace_file(agent, "TOOL_GOTCHAS.org");

    g_assert_nonnull(agents_org);
    g_assert_nonnull(tools);
    g_assert_nonnull(gotchas);

    /* It does not block. */
    g_assert_null(strstr(agents_org, "blocks until the other"));
    g_assert_null(strstr(tools, "Send and *wait* for the reply"));

    /* There is no timeout, so there is nothing to time out. */
    g_assert_null(strstr(gotchas, "can time out"));

    /*
     * And the true shape is said, not merely un-said: an agent told
     * nothing about how the answer comes back goes looking in its
     * mailbox, which delivery has already emptied.
     */
    g_assert_nonnull(strstr(tools, "does not wait"));
    g_assert_nonnull(strstr(gotchas, "does not wait"));

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
 * Skills come from the workspace, not the state directory.
 *
 * Those are the same directory for every agent that does not set
 * `agents.workspace`, which is why building skills.dir from the state
 * dir was invisible for as long as it was. An agent given a workspace of
 * its own had skills.dir naming a directory clawtilla never creates and
 * nothing in the tree ever writes to -- so it silently had no skills at
 * all, while the config looked perfectly reasonable.
 */
static void
test_skills_come_from_the_workspace(void)
{
    Fixture fixture = { 0 };
    ClawtAgentConfig *agent;
    g_autofree gchar *rendered = NULL;
    g_autofree gchar *state_skills = NULL;

    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: scribe\n"
                  "    workspace: \"/tmp/clawt-elsewhere\"\n");
    agent = first_agent(&fixture);

    rendered = clawt_config_render_agent(fixture.config, agent,
                                         "/tmp/agents.sock", fixture.dir,
                                         NULL);
    g_assert_nonnull(rendered);

    /* The workspace it was given... */
    g_assert_nonnull(strstr(rendered, "\"/tmp/clawt-elsewhere/skills\""));

    /* ...and never the state directory, which holds no skills. */
    state_skills = g_build_filename(fixture.dir, "skills", NULL);
    g_assert_null(strstr(rendered, state_skills));

    fixture_teardown(&fixture);
}

/*
 * A list set through the config API round-trips as a sequence.
 *
 * It used to be written as a scalar, which the reader refuses -- so the
 * value was saved to the file and then read back as the schema default,
 * with every surface reporting that it had been set.
 */
static void
test_string_list_round_trips(void)
{
    Fixture fixture = { 0 };
    ClawtAgentConfig *agent;
    const gchar *const wanted[] = { "SOUL.md", "USER.md", NULL };
    g_auto(GStrv) read_back = NULL;

    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: scribe\n");
    agent = first_agent(&fixture);

    g_assert_true(clawt_agent_config_set_string_list(
        agent, "persona.identity_files", wanted));

    read_back = clawt_agent_config_get_string_list(agent,
                                                   "persona.identity_files");

    g_assert_nonnull(read_back);
    g_assert_cmpuint(g_strv_length(read_back), ==, 2);
    g_assert_cmpstr(read_back[0], ==, "SOUL.md");
    g_assert_cmpstr(read_back[1], ==, "USER.md");

    fixture_teardown(&fixture);
}

/*
 * Scaffolding writes no identity file that nothing will load.
 *
 * The blank .org set beside an imported persona is the whole of the
 * import bug: seven templates saying "/(fill in)/" next to the real
 * files, and the templates are the ones the agent read.
 */
static void
test_scaffold_skips_unloaded_identity_files(void)
{
    Fixture fixture = { 0 };
    ClawtAgentConfig *agent;
    g_autofree gchar *workspace = NULL;
    g_autofree gchar *soul_org = NULL;
    g_autofree gchar *tools_org = NULL;
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: scribe\n"
                  "    persona:\n"
                  "      identity_files: [\"TOOLS.org\"]\n");
    agent = first_agent(&fixture);

    g_assert_true(clawt_workspace_scaffold(agent, &error));
    g_assert_no_error(error);

    workspace = clawt_agent_config_get_workspace(agent);
    soul_org = g_build_filename(workspace, "SOUL.org", NULL);
    tools_org = g_build_filename(workspace, "TOOLS.org", NULL);

    /* The one that is loaded is written; the six that are not are not. */
    g_assert_true(g_file_test(tools_org, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(soul_org, G_FILE_TEST_EXISTS));

    fixture_teardown(&fixture);
}

/*
 * A workspace that already has a markdown persona is detected.
 */
static void
test_detects_an_existing_markdown_persona(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *soul = NULL;
    g_autofree gchar *user = NULL;
    g_auto(GStrv) found = NULL;

    fixture_setup(&fixture, "agents:\n  - id: scribe\n");

    soul = g_build_filename(fixture.dir, "SOUL.md", NULL);
    user = g_build_filename(fixture.dir, "USER.md", NULL);
    g_assert_true(g_file_set_contents(soul, "who I am", -1, NULL));
    g_assert_true(g_file_set_contents(user, "who Ben is", -1, NULL));

    found = clawt_workspace_detect_identity_files(fixture.dir);

    g_assert_nonnull(found);
    g_assert_cmpuint(g_strv_length(found), ==, 2);
    /* Load order is the table's, not the directory's. */
    g_assert_cmpstr(found[0], ==, "SOUL.md");
    g_assert_cmpstr(found[1], ==, "USER.md");

    fixture_teardown(&fixture);
}

/*
 * ...but a workspace that already speaks org has made its choice, and
 * adopting markdown beside it would load the same concern twice.
 */
static void
test_detects_nothing_when_org_is_present(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *soul_md = NULL;
    g_autofree gchar *soul_org = NULL;
    g_auto(GStrv) found = NULL;

    fixture_setup(&fixture, "agents:\n  - id: scribe\n");

    soul_md = g_build_filename(fixture.dir, "SOUL.md", NULL);
    soul_org = g_build_filename(fixture.dir, "SOUL.org", NULL);
    g_assert_true(g_file_set_contents(soul_md, "markdown", -1, NULL));
    g_assert_true(g_file_set_contents(soul_org, "org", -1, NULL));

    found = clawt_workspace_detect_identity_files(fixture.dir);

    g_assert_null(found);

    fixture_teardown(&fixture);
}

/*
 * An empty directory has no persona to adopt, which must read as
 * "nothing here" rather than as an empty list somebody could set.
 */
static void
test_detects_nothing_in_an_empty_workspace(void)
{
    Fixture fixture = { 0 };
    g_auto(GStrv) found = NULL;

    fixture_setup(&fixture, "agents:\n  - id: scribe\n");

    found = clawt_workspace_detect_identity_files(fixture.dir);

    g_assert_null(found);

    fixture_teardown(&fixture);
}

/*
 * Every STRING_LIST an agent can hold round-trips, not just the one the
 * bug was found through.
 *
 * The report reached this through persona.identity_files. The same
 * handler writes eight more, and two of them -- computer.host.allow_paths
 * and deny_paths -- are confinement: a denylist accepted, saved, and read
 * back as nothing is a sandbox that reports success and confines
 * nothing. That is the sharp end of the same defect, and it is worth a
 * test naming it rather than trusting that one example covers the set.
 */
static void
test_every_agent_list_round_trips(void)
{
    static const gchar *const keys[] = {
        "persona.identity_files",
        "model.fallbacks",
        "computer.host.allow_paths",
        "computer.host.deny_paths",
        "tools.allow",
        "tools.deny",
        NULL
    };
    const gchar *const wanted[] = { "one", "two", NULL };
    Fixture fixture = { 0 };
    ClawtAgentConfig *agent;
    gsize i;

    fixture_setup(&fixture, "agents:\n  - id: scribe\n");
    agent = first_agent(&fixture);

    for (i = 0; keys[i] != NULL; i++) {
        g_auto(GStrv) read_back = NULL;

        g_assert_true(clawt_agent_config_set_string_list(agent, keys[i],
                                                         wanted));

        read_back = clawt_agent_config_get_string_list(agent, keys[i]);

        if (read_back == NULL)
            g_test_fail_printf("%s was accepted and read back as nothing",
                               keys[i]);
        else
            g_assert_cmpstr(read_back[0], ==, "one");
    }

    fixture_teardown(&fixture);
}

/*
 * Clearing a list reaches the reader as "unset", not as an empty list.
 *
 * The difference matters for the confinement keys: an empty deny_paths
 * and an absent one both deny nothing, but an empty allow_paths would
 * allow nothing, and the two must not be confused by a client that
 * cleared a field.
 */
static void
test_clearing_a_list_unsets_it(void)
{
    const gchar *const wanted[] = { "one", NULL };
    Fixture fixture = { 0 };
    ClawtAgentConfig *agent;
    g_auto(GStrv) read_back = NULL;

    fixture_setup(&fixture, "agents:\n  - id: scribe\n");
    agent = first_agent(&fixture);

    g_assert_true(clawt_agent_config_set_string_list(agent, "tools.allow",
                                                     wanted));
    g_assert_true(clawt_agent_config_set_string_list(agent, "tools.allow",
                                                     NULL));

    read_back = clawt_agent_config_get_string_list(agent, "tools.allow");
    g_assert_null(read_back);

    fixture_teardown(&fixture);
}

/*
 * A list written as a lone scalar is one element, not nothing.
 *
 * `allow_paths: /srv/data` is what a person writes by hand, and it is
 * what every editor's autocomplete produces for a key it has only ever
 * seen hold one thing. node_to_strv() refused it -- it takes a YAML
 * sequence and nothing else -- so the value was parsed, discarded, and
 * the key read back as its schema default.
 *
 * For allow_paths that default is unset, and an unset allowlist is an
 * empty one. The confinement an operator wrote was not narrowed or
 * widened; it was dropped, and nothing anywhere said so.
 */
static void
test_a_lone_scalar_is_a_one_element_list(void)
{
    Fixture fixture = { 0 };
    ClawtAgentConfig *agent;
    g_auto(GStrv) read_back = NULL;

    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: scribe\n"
                  "    computer:\n"
                  "      host:\n"
                  "        allow_paths: /srv/data\n");
    agent = first_agent(&fixture);

    read_back = clawt_agent_config_get_string_list(
        agent, "computer.host.allow_paths");

    if (read_back == NULL)
        g_test_fail_printf("a hand-written allow_paths scalar read back "
                           "as nothing");
    else {
        g_assert_cmpstr(read_back[0], ==, "/srv/data");
        g_assert_null(read_back[1]);
    }

    fixture_teardown(&fixture);
}

/*
 * An empty scalar still means unset, not a list holding "".
 *
 * The distinction is the one test_clearing_a_list_unsets_it() protects,
 * and accepting a lone scalar must not blur it: `tools.allow:` with
 * nothing after it is a key somebody started and left, not a grant of
 * no tools at all.
 */
static void
test_an_empty_scalar_is_still_unset(void)
{
    Fixture fixture = { 0 };
    ClawtAgentConfig *agent;
    g_auto(GStrv) read_back = NULL;

    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: scribe\n"
                  "    tools:\n"
                  "      allow:\n");
    agent = first_agent(&fixture);

    read_back = clawt_agent_config_get_string_list(agent, "tools.allow");
    g_assert_null(read_back);

    fixture_teardown(&fixture);
}

/*
 * Setting from a string asks the schema what the key is.
 *
 * Every caller that takes a value as text -- the CLI, the create_agent
 * tool -- had to decide for itself whether that text was a list, and a
 * caller that did not decide wrote a scalar the reader then threw away.
 * There is one answer and the schema has it, so there is one place that
 * asks.
 */
static void
test_set_from_string_follows_the_schema(void)
{
    Fixture fixture = { 0 };
    ClawtAgentConfig *agent;
    g_auto(GStrv) read_back = NULL;

    fixture_setup(&fixture, "agents:\n  - id: scribe\n");
    agent = first_agent(&fixture);

    /* A STRING_LIST key: comma-separated in, sequence out. */
    g_assert_true(clawt_agent_config_set_from_string(agent, "tools.allow",
                                                     "read, write"));

    read_back = clawt_agent_config_get_string_list(agent, "tools.allow");

    if (read_back == NULL)
        g_test_fail_printf("tools.allow was accepted and read back as "
                           "nothing");
    else {
        g_assert_cmpstr(read_back[0], ==, "read");
        g_assert_cmpstr(read_back[1], ==, "write");
        g_assert_null(read_back[2]);
    }

    /* A STRING key is left alone: a comma is part of the value. */
    g_assert_true(clawt_agent_config_set_from_string(agent, "name",
                                                     "Scribe, the second"));
    g_assert_cmpstr(clawt_agent_config_get_string(agent, "name"), ==,
                    "Scribe, the second");

    fixture_teardown(&fixture);
}

/*
 * The scaffolder and the renderer ask the same question.
 *
 * They each used to decide for themselves what an agent loads, and a
 * workspace full of files nobody reads is what that produces. One
 * function answers now, and this asserts the two agree for the case that
 * distinguishes them.
 */
static void
test_scaffold_and_renderer_agree(void)
{
    Fixture fixture = { 0 };
    ClawtAgentConfig *agent;
    g_auto(GStrv) effective = NULL;
    g_autofree gchar *rendered = NULL;

    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: scribe\n"
                  "    persona:\n"
                  "      identity_files: [\"SOUL.md\", \"NOTES.md\"]\n");
    agent = first_agent(&fixture);

    effective = clawt_workspace_effective_identity_files(agent);
    g_assert_nonnull(effective);
    g_assert_cmpstr(effective[0], ==, "SOUL.md");

    rendered = clawt_config_render_agent(fixture.config, agent,
                                         "/tmp/agents.sock", fixture.dir,
                                         NULL);
    g_assert_nonnull(rendered);
    g_assert_nonnull(strstr(rendered, "\"SOUL.md\""));
    g_assert_nonnull(strstr(rendered, "\"NOTES.md\""));

    /* And nothing from the standard set it did not ask for. */
    g_assert_null(strstr(rendered, "\"IDENTITY.org\""));

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
        fixture.config, agent, "/run/clawtilla.sock", "/state/scribe", &error));
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
 * A VM agent granted a desktop is given the relay that reaches it.
 *
 * The entry names the clawtilla CLI rather than ssh, because the port
 * that reaches the guest is chosen when the VM is provisioned -- after
 * this file is written.
 */
static void
test_mcp_config_offers_a_guest_desktop(void)
{
    Fixture fixture = { 0 };
    ClawtAgentConfig *agent;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *path = NULL;
    g_autofree gchar *text = NULL;

    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: scribe\n"
                  "    computer:\n"
                  "      type: vm\n"
                  "      desktop:\n"
                  "        enabled: true\n");
    agent = first_agent(&fixture);

    g_assert_true(clawt_workspace_scaffold(agent, &error));
    g_assert_true(clawt_workspace_write_mcp_config(
        fixture.config, agent, "/run/clawtilla.sock", "/state/scribe", &error));
    g_assert_no_error(error);

    path = clawt_workspace_file_path(agent, ".mcp.json");
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));

    g_assert_nonnull(strstr(text, "clawtilla-desktop"));
    g_assert_nonnull(strstr(text, "desktop-mcp"));

    /*
     * --socket before the verb.  The CLI splits its own options from the
     * subcommand's at the first verb it recognises, so one written after
     * `computer` is handed to a subcommand that has never heard of it.
     */
    {
        const gchar *socket_at = strstr(text, "--socket");
        const gchar *verb_at = strstr(text, "\"computer\"");

        g_assert_nonnull(socket_at);
        g_assert_nonnull(verb_at);
        g_assert_true(socket_at < verb_at);
    }

    fixture_teardown(&fixture);
}

/*
 * ...and an agent without one is not.  A desktop entry left behind would
 * start an ssh to a VM that is not there and fail on every tool call.
 */
static void
test_mcp_config_has_no_desktop_without_a_vm(void)
{
    Fixture fixture = { 0 };
    ClawtAgentConfig *agent;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *path = NULL;
    g_autofree gchar *text = NULL;

    /*
     * The grant without the VM.  Both halves are needed: a host desktop
     * is reached without any of this.
     */
    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: scribe\n"
                  "    computer:\n"
                  "      type: host\n"
                  "      desktop:\n"
                  "        enabled: true\n");
    agent = first_agent(&fixture);

    g_assert_true(clawt_workspace_scaffold(agent, &error));
    g_assert_true(clawt_workspace_write_mcp_config(
        fixture.config, agent, "/run/clawtilla.sock", "/state/scribe", &error));

    path = clawt_workspace_file_path(agent, ".mcp.json");
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));

    g_assert_null(strstr(text, "clawtilla-desktop"));

    fixture_teardown(&fixture);
}

/*
 * Turning the desktop off takes the entry away again.
 *
 * The whole of the rest of this file is carried across untouched, so an
 * entry clawtilla owns has to be actively removed -- otherwise revoking
 * the grant leaves the relay in place for ever.
 */
static void
test_mcp_config_drops_a_desktop_that_was_revoked(void)
{
    Fixture fixture = { 0 };
    ClawtAgentConfig *agent;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *path = NULL;
    g_autofree gchar *text = NULL;

    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: scribe\n"
                  "    computer:\n"
                  "      type: vm\n"
                  "      desktop:\n"
                  "        enabled: true\n");
    agent = first_agent(&fixture);

    g_assert_true(clawt_workspace_scaffold(agent, &error));
    g_assert_true(clawt_workspace_write_mcp_config(
        fixture.config, agent, "/run/clawtilla.sock", "/state/scribe", &error));

    path = clawt_workspace_file_path(agent, ".mcp.json");
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
    g_assert_nonnull(strstr(text, "clawtilla-desktop"));
    g_clear_pointer(&text, g_free);

    clawt_agent_config_set_string(agent, "computer.desktop.enabled",
                                  "false");

    g_assert_true(clawt_workspace_write_mcp_config(
        fixture.config, agent, "/run/clawtilla.sock", "/state/scribe", &error));
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));

    g_assert_null(strstr(text, "clawtilla-desktop"));

    /* The one clawtilla always owns is still there. */
    g_assert_nonnull(strstr(text, "clawtilla-mcp-server"));

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
    g_assert_true(clawt_workspace_write_mcp_config(
        fixture.config, agent, "/old.sock",
                                                    "/state/scribe", &error));
    g_assert_true(clawt_workspace_write_mcp_config(
        fixture.config, agent, "/new.sock",
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
    g_assert_true(clawt_workspace_write_mcp_config(
        fixture.config, agent, "/old.sock",
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

    g_assert_true(clawt_workspace_write_mcp_config(
        fixture.config, agent, "/new.sock",
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
    g_assert_true(clawt_workspace_write_mcp_config(
        fixture.config, agent, "/run/c.sock",
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
    g_assert_true(clawt_workspace_write_mcp_config(
        fixture.config, agent, "/run/c.sock",
                                                    "/state/scribe", &error));

    path = clawt_workspace_file_path(agent, ".mcp.json");

    /* Backdated, so "unchanged" is a fact rather than a timer race. */
    backdated.actime = 1000000;
    backdated.modtime = 1000000;
    g_assert_cmpint(g_utime(path, &backdated), ==, 0);
    g_assert_cmpint(g_stat(path, &before), ==, 0);

    g_assert_true(clawt_workspace_write_mcp_config(
        fixture.config, agent, "/run/c.sock",
                                                    "/state/scribe", &error));
    g_assert_no_error(error);

    g_assert_cmpint(g_stat(path, &after), ==, 0);
    g_assert_cmpint(before.st_mtime, ==, after.st_mtime);

    fixture_teardown(&fixture);
}


/*
 * The org files are the agent's to keep current, and it has to be told
 * so: clawtilla writes them once and then leaves them alone, and
 * everything an agent learns is lost at the end of a conversation
 * unless it lands in one of them.
 */
static void
test_the_agent_is_told_to_maintain_its_own_files(void)
{
    Fixture fixture = { 0 };
    ClawtAgentConfig *agent;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *agents = NULL;

    fixture_setup(&fixture, "agents:\n  - id: scribe\n");
    agent = first_agent(&fixture);

    g_assert_true(clawt_workspace_scaffold(agent, &error));
    g_assert_no_error(error);

    /*
     * TOOLS.org, beside the memory guidance it is deliberately
     * contrasted with -- and it is the file that carries a
     * clawtilla-managed region, so the warning about not editing inside
     * one is in the file that has one.
     */
    agents = read_workspace_file(agent, "TOOLS.org");

    g_assert_nonnull(strstr(agents, "yours to edit"));
    g_assert_nonnull(strstr(agents, "TOOL_GOTCHAS.org"));

    /*
     * With the boundary, because the alternative is an agent editing
     * inside a region that is rewritten on every start and losing the
     * work without being told.
     */
    g_assert_nonnull(strstr(agents, "BEGIN clawtilla"));
    g_assert_nonnull(strstr(agents, ".mcp.json"));

    /*
     * And the reason, not just the rule. A rule with its reason survives
     * being read by a version of this agent that has forgotten
     * everything else.
     */
    g_assert_nonnull(strstr(agents, "Write the reason"));

    fixture_teardown(&fixture);
}

/*
 * The purpose an operator wrote when creating the agent becomes the
 * agent's mission, in the one file that decides what it does when
 * nobody has told it what to do.
 *
 * It used to be written to a `persona` key nothing reads, so the whole
 * persona was discarded and the new agent started with nothing but the
 * scaffold.  Nothing warned, which is the part that cost a day.
 */
static void
test_scaffold_writes_the_purpose_as_the_mission(void)
{
    Fixture fixture = { 0 };
    ClawtAgentConfig *agent;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *soul = NULL;
    gboolean written = FALSE;

    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: scribe\n"
                  "    description: writes things down\n");
    agent = first_agent(&fixture);

    g_assert_true(clawt_workspace_scaffold_with_mission(
        agent, "You keep the notes. Never speak first.", &written, &error));
    g_assert_no_error(error);
    g_assert_true(written);

    soul = read_workspace_file(agent, "SOUL.org");
    g_assert_nonnull(soul);
    g_assert_nonnull(strstr(soul, "You keep the notes. Never speak first."));

    /*
     * And the nudge to rewrite the generated line goes with it: it is
     * addressed to an agent whose mission was generated from a config
     * field, and this one's was written by a person.
     */
    g_assert_null(strstr(soul, "Rewrite the line above"));

    /* The rest of the set is still scaffolded around it. */
    g_assert_nonnull(strstr(soul, "* Operating Parameters"));
    {
        g_autofree gchar *identity = read_workspace_file(agent,
                                                          "IDENTITY.org");
        g_assert_nonnull(identity);
    }

    fixture_teardown(&fixture);
}

/*
 * With no mission the description still fills the slot, and the nudge
 * stays -- an agent whose mission was generated needs to be told so.
 */
static void
test_scaffold_without_a_mission_keeps_the_nudge(void)
{
    Fixture fixture = { 0 };
    ClawtAgentConfig *agent;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *soul = NULL;

    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: scribe\n"
                  "    description: writes things down\n");
    agent = first_agent(&fixture);

    g_assert_true(clawt_workspace_scaffold(agent, &error));
    g_assert_no_error(error);

    soul = read_workspace_file(agent, "SOUL.org");
    g_assert_nonnull(soul);
    g_assert_nonnull(strstr(soul, "writes things down"));
    g_assert_nonnull(strstr(soul, "Rewrite the line above"));

    fixture_teardown(&fixture);
}

/*
 * A SOUL.org that is already there wins, because it is somebody's work
 * -- but the caller is told the mission did not land rather than left
 * believing it did.  Silently losing it is the whole defect.
 *
 * Both directions in one test, deliberately.  Asserting only that a
 * pre-existing SOUL.org reports FALSE passes just as well when the
 * mission is never written at all, so on its own it proves nothing: it
 * is exactly the test that would have survived this fix being reverted.
 * The positive control below is what makes the negative one mean
 * something.
 */
static void
test_scaffold_says_when_the_mission_did_not_land(void)
{
    Fixture fixture = { 0 };
    ClawtAgentConfig *agent;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *path = NULL;
    g_autofree gchar *landed = NULL;
    g_autofree gchar *after = NULL;
    gboolean written = FALSE;

    fixture_setup(&fixture, "agents:\n  - id: scribe\n");
    agent = first_agent(&fixture);

    g_assert_true(clawt_workspace_scaffold(agent, &error));
    g_assert_no_error(error);

    path = clawt_workspace_file_path(agent, "SOUL.org");

    /*
     * Positive control: with no SOUL.org in the way the mission lands,
     * and the caller is told so.
     */
    g_assert_cmpint(g_unlink(path), ==, 0);

    g_assert_true(clawt_workspace_scaffold_with_mission(
        agent, "You keep the notes.", &written, &error));
    g_assert_no_error(error);
    g_assert_true(written);

    g_assert_true(g_file_get_contents(path, &landed, NULL, NULL));
    g_assert_nonnull(strstr(landed, "You keep the notes."));

    /*
     * And now the case this exists for: the file is somebody's, so it
     * is left alone and the caller is told the purpose is not in it.
     */
    g_assert_true(g_file_set_contents(path, "MINE\n", -1, NULL));

    written = TRUE;

    g_assert_true(clawt_workspace_scaffold_with_mission(
        agent, "You keep the notes.", &written, &error));
    g_assert_no_error(error);
    g_assert_false(written);

    g_assert_true(g_file_get_contents(path, &after, NULL, NULL));
    g_assert_cmpstr(after, ==, "MINE\n");

    fixture_teardown(&fixture);
}


/* ── Importing an existing workspace ─────────────────────────────── */

/*
 * A directory of this test's own, removed when it is done.
 *
 * g_get_user_data_dir() is the *developer's* real ~/.local/share here:
 * this binary does not redirect XDG_DATA_HOME, so building paths under
 * it wrote import-src, import-link and friends into somebody's actual
 * data directory -- and the second run then failed, because a workspace
 * that already exists is refused. That is the fixture trap this project
 * has recorded once already, arrived at from a different direction.
 */
typedef gchar *ImportScratch;

static void
import_scratch_clear(ImportScratch *dir)
{
    if (dir == NULL || *dir == NULL)
        return;

    /*
     * Fenced by its own root, so a symlink inside it -- which is the
     * whole subject of these tests -- cannot carry the removal out.
     */
    clawt_remove_tree(*dir, *dir, NULL);
    g_rmdir(*dir);
    g_clear_pointer(dir, g_free);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(ImportScratch, import_scratch_clear)

static ImportScratch
import_scratch(void)
{
    gchar *dir = g_dir_make_tmp("clawt-import-XXXXXX", NULL);

    g_assert_nonnull(dir);

    return dir;
}


/*
 * A link is a link, and the original is not copied.
 *
 * This is the whole point of the mode: somebody maintaining a workspace
 * somewhere they like it wants clawtilla to *use* it, not to take a
 * snapshot that diverges the moment either side is edited.
 */
static void
test_a_linked_workspace_points_at_the_original(void)
{
    g_auto(ImportScratch) scratch = import_scratch();
    g_autofree gchar *source = NULL;
    g_autofree gchar *workspace = NULL;
    g_autofree gchar *detail = NULL;
    g_autofree gchar *marker = NULL;
    g_autofree gchar *seen = NULL;
    g_autoptr(GError) error = NULL;
    guint files = 99;

    source = g_build_filename(scratch, "import-src", NULL);
    workspace = g_build_filename(scratch, "import-link", NULL);
    g_assert_cmpint(g_mkdir_with_parents(source, 0700), ==, 0);

    marker = g_build_filename(source, "SOUL.org", NULL);
    g_assert_true(g_file_set_contents(marker, "a persona\n", -1, NULL));

    g_assert_true(clawt_workspace_adopt(CLAWT_IMPORT_LINK, source, workspace,
                                        FALSE, &files, &detail, &error));
    g_assert_no_error(error);

    g_assert_true(g_file_test(workspace, G_FILE_TEST_IS_SYMLINK));
    g_assert_cmpuint(files, ==, 0);
    g_assert_nonnull(detail);
    g_assert_nonnull(strstr(detail, "stays where it is"));

    /* And writing through the link reaches the original. */
    {
        g_autofree gchar *through = g_build_filename(workspace, "NOTES.org",
                                                     NULL);
        g_autofree gchar *original = g_build_filename(source, "NOTES.org",
                                                      NULL);

        g_assert_true(g_file_set_contents(through, "written\n", -1, NULL));
        g_assert_true(g_file_get_contents(original, &seen, NULL, NULL));
        g_assert_cmpstr(seen, ==, "written\n");
    }
}

/*
 * A workspace already there is refused, not replaced.
 *
 * It holds an agent's persona, and turning it into a link to somewhere
 * else would discard that with nothing said.
 */
static void
test_linking_over_an_existing_workspace_is_refused(void)
{
    g_auto(ImportScratch) scratch = import_scratch();
    g_autofree gchar *source = NULL;
    g_autofree gchar *workspace = NULL;
    g_autoptr(GError) error = NULL;

    source = g_build_filename(scratch, "import-src2", NULL);
    workspace = g_build_filename(scratch, "import-taken", NULL);
    g_assert_cmpint(g_mkdir_with_parents(source, 0700), ==, 0);
    g_assert_cmpint(g_mkdir_with_parents(workspace, 0700), ==, 0);

    g_assert_false(clawt_workspace_adopt(CLAWT_IMPORT_LINK, source,
                                         workspace, FALSE, NULL, NULL,
                                         &error));
    g_assert_nonnull(error);
    g_assert_true(g_error_matches(error, CLAWT_ERROR,
                                  CLAWT_ERROR_ALREADY_EXISTS));
}

/*
 * Removing a linked agent removes the link and leaves the directory.
 *
 * clawt_remove_tree() resolves the path it is given and refuses
 * anything outside the root, so before it learned to unlink a symlink
 * this failed for exactly the agents where *following* the link would
 * have been worst: `agent rm --purge` errored, and the alternative
 * would have deleted somebody's real workspace.
 */
static void
test_removing_a_linked_workspace_leaves_the_original(void)
{
    g_auto(ImportScratch) scratch = import_scratch();
    g_autofree gchar *root = NULL;
    g_autofree gchar *source = NULL;
    g_autofree gchar *workspace = NULL;
    g_autofree gchar *marker = NULL;
    g_autoptr(GError) error = NULL;

    root = g_build_filename(scratch, "import-root", NULL);
    source = g_build_filename(scratch, "import-src3", NULL);
    workspace = g_build_filename(root, "linked", NULL);

    g_assert_cmpint(g_mkdir_with_parents(root, 0700), ==, 0);
    g_assert_cmpint(g_mkdir_with_parents(source, 0700), ==, 0);

    marker = g_build_filename(source, "SOUL.org", NULL);
    g_assert_true(g_file_set_contents(marker, "keep me\n", -1, NULL));

    g_assert_true(clawt_workspace_adopt(CLAWT_IMPORT_LINK, source, workspace,
                                        FALSE, NULL, NULL, &error));
    g_assert_no_error(error);

    g_assert_true(clawt_remove_tree(workspace, root, &error));
    g_assert_no_error(error);

    g_assert_false(g_file_test(workspace, G_FILE_TEST_EXISTS));
    g_assert_true(g_file_test(marker, G_FILE_TEST_EXISTS));
}

/*
 * A copy says it is a copy, and the original is untouched.
 *
 * The detail sentence is what a client shows, and the difference
 * between a fork and a link is the thing somebody most needs told.
 */
static void
test_a_copy_says_the_original_is_separate(void)
{
    g_auto(ImportScratch) scratch = import_scratch();
    g_autofree gchar *source = NULL;
    g_autofree gchar *workspace = NULL;
    g_autofree gchar *detail = NULL;
    g_autofree gchar *marker = NULL;
    g_autoptr(GError) error = NULL;
    guint files = 0;

    source = g_build_filename(scratch, "import-src4", NULL);
    workspace = g_build_filename(scratch, "import-copy", NULL);
    g_assert_cmpint(g_mkdir_with_parents(source, 0700), ==, 0);

    marker = g_build_filename(source, "SOUL.org", NULL);
    g_assert_true(g_file_set_contents(marker, "a persona\n", -1, NULL));

    g_assert_true(clawt_workspace_adopt(CLAWT_IMPORT_COPY, source, workspace,
                                        FALSE, &files, &detail, &error));
    g_assert_no_error(error);

    g_assert_false(g_file_test(workspace, G_FILE_TEST_IS_SYMLINK));
    g_assert_cmpuint(files, >, 0);
    g_assert_nonnull(strstr(detail, "separate copy"));
}

/*
 * A source that is not there is refused with the path in it.
 *
 * Both directory modes, because the check moved out of the daemon when
 * git arrived -- and a mode that lost it would create the config entry
 * and then fail somewhere less obvious.
 */
static void
test_a_missing_source_is_refused_by_both_directory_modes(void)
{
    g_auto(ImportScratch) scratch = import_scratch();
    static const ClawtImportMode modes[] = {
        CLAWT_IMPORT_COPY, CLAWT_IMPORT_LINK
    };
    guint i;

    for (i = 0; i < G_N_ELEMENTS(modes); i++) {
        g_autofree gchar *workspace = NULL;
        g_autoptr(GError) error = NULL;

        workspace = g_build_filename(scratch, "import-none", NULL);

        g_assert_false(clawt_workspace_adopt(
            modes[i], "/nonexistent/clawt-import", workspace, FALSE, NULL,
            NULL, &error));
        g_assert_nonnull(error);
        g_assert_true(g_error_matches(error, CLAWT_ERROR,
                                      CLAWT_ERROR_NOT_FOUND));
        g_assert_nonnull(strstr(error->message, "/nonexistent/clawt-import"));
    }
}

/*
 * The modes both clients offer, and their round trip.
 *
 * A nick that did not survive to_nick(from_nick(x)) would be a mode a
 * client could select and the daemon would refuse.
 */
static void
test_every_import_mode_round_trips(void)
{
    guint i;

    g_assert_cmpuint(clawt_import_mode_count(), ==, 3);

    for (i = 0; i < clawt_import_mode_count(); i++) {
        const gchar *nick = clawt_import_mode_nth_nick(i);

        g_assert_nonnull(nick);
        g_assert_nonnull(clawt_import_mode_nth_label(i));
        g_assert_cmpint(clawt_import_mode_from_nick(nick), ==,
                        clawt_import_mode_nth(i));
    }

    /* Unrecognised is the safest mode, never the most destructive. */
    g_assert_cmpint(clawt_import_mode_from_nick("lnik"), ==,
                    CLAWT_IMPORT_COPY);
    g_assert_cmpint(clawt_import_mode_from_nick(NULL), ==, CLAWT_IMPORT_COPY);

    /* Only the git mode wants a URL rather than a directory. */
    g_assert_true(clawt_import_mode_takes_url(CLAWT_IMPORT_GIT));
    g_assert_false(clawt_import_mode_takes_url(CLAWT_IMPORT_COPY));
    g_assert_false(clawt_import_mode_takes_url(CLAWT_IMPORT_LINK));
}

/*
 * Whether a git import can be a submodule is a property of the machine.
 *
 * A directory inside a repository gets one; a directory outside any
 * repository gets a plain clone. The decision is asked here rather than
 * inside the clone, so it can be checked without the network.
 */
static void
test_the_submodule_decision_follows_the_repository(void)
{
    g_auto(ImportScratch) scratch = import_scratch();
    g_autofree gchar *repo = NULL;
    g_autofree gchar *inside = NULL;
    g_autofree gchar *outside = NULL;
    g_autofree gchar *found = NULL;
    const gchar *argv[] = { "git", "init", "-q", NULL, NULL };
    gint status = 0;

    {
        /* transfer full, so it has to be held to be freed. */
        g_autofree gchar *git = g_find_program_in_path("git");

        if (git == NULL) {
            g_test_skip("git is not installed");
            return;
        }
    }

    repo = g_build_filename(scratch, "import-repo", NULL);
    g_assert_cmpint(g_mkdir_with_parents(repo, 0700), ==, 0);

    argv[3] = repo;
    g_assert_true(g_spawn_sync(NULL, (gchar **)argv, NULL,
                               G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL,
                               &status, NULL));
    g_assert_true(g_spawn_check_wait_status(status, NULL));

    /* A workspace that does not exist yet still resolves through its
     * nearest existing ancestor -- which is the case that matters,
     * because the question is asked before anything is created. */
    inside = g_build_filename(repo, "agents", "scribe", NULL);
    found = clawt_workspace_git_toplevel(inside);
    g_assert_nonnull(found);

    outside = g_build_filename(scratch, "import-not-a-repo", NULL);
    g_assert_cmpint(g_mkdir_with_parents(outside, 0700), ==, 0);

    /*
     * Only meaningful when the temporary data directory is not itself
     * inside somebody's repository -- which it is not under the test
     * harness, but saying so beats an assertion that fails for a reason
     * unrelated to the feature.
     */
    {
        g_autofree gchar *elsewhere =
            clawt_workspace_git_toplevel(outside);

        if (elsewhere != NULL)
            g_test_message("the test data directory is inside a git "
                           "repository; the negative half was skipped");
        else
            g_assert_null(elsewhere);
    }
}

/*
 * Makes a git repository with a persona in it, for the import tests.
 *
 * Local paths only, so this is hermetic -- no network, and `git clone`
 * of a directory needs none.
 */
static gboolean
make_source_repo(const gchar *path)
{
    /*
     * Wide enough for the longest row *plus* its NULL. The commit line
     * is exactly eight arguments, so an [8] here would leave it
     * unterminated -- g_spawn_sync would then walk off the end of the
     * row into the next one.
     */
    static const gchar *const steps[][10] = {
        { "git", "init", "-q", NULL },
        { "git", "add", "-A", NULL },
        { "git", "-c", "user.email=t@example", "-c", "user.name=t",
          "commit", "-qm", "persona", NULL }
    };
    g_autofree gchar *soul = NULL;
    guint i;

    if (g_mkdir_with_parents(path, 0700) != 0)
        return FALSE;

    soul = g_build_filename(path, "SOUL.org", NULL);

    if (!g_file_set_contents(soul, "* Scribe\n", -1, NULL))
        return FALSE;

    for (i = 0; i < G_N_ELEMENTS(steps); i++) {
        gint status = 0;

        if (!g_spawn_sync(path, (gchar **)steps[i], NULL,
                          G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                          G_SPAWN_STDERR_TO_DEV_NULL,
                          NULL, NULL, NULL, NULL, &status, NULL))
            return FALSE;

        if (!g_spawn_check_wait_status(status, NULL))
            return FALSE;
    }

    return TRUE;
}

static gboolean
have_git(void)
{
    /* transfer full, so it has to be held to be freed. */
    g_autofree gchar *git = g_find_program_in_path("git");

    return git != NULL;
}

/*
 * A git import outside any repository is an ordinary clone, and says so.
 *
 * The sentence matters as much as the clone: this is the outcome a
 * caller could not predict, and reporting a submodule here -- or saying
 * nothing -- would leave somebody believing their state repo tracks a
 * workspace it has never heard of.
 */
static void
test_a_git_import_outside_a_repository_clones(void)
{
    g_auto(ImportScratch) scratch = import_scratch();
    g_autofree gchar *source = NULL;
    g_autofree gchar *workspace = NULL;
    g_autofree gchar *detail = NULL;
    g_autofree gchar *soul = NULL;
    g_autoptr(GError) error = NULL;

    if (!have_git()) {
        g_test_skip("git is not installed");
        return;
    }

    source = g_build_filename(scratch, "source", NULL);
    workspace = g_build_filename(scratch, "agents", "scribe", NULL);

    g_assert_true(make_source_repo(source));

    g_assert_true(clawt_workspace_adopt(CLAWT_IMPORT_GIT, source, workspace,
                                        FALSE, NULL, &detail, &error));
    g_assert_no_error(error);

    /* The persona came across, and it is a repository. */
    soul = g_build_filename(workspace, "SOUL.org", NULL);
    g_assert_true(g_file_test(soul, G_FILE_TEST_EXISTS));

    {
        g_autofree gchar *dot_git = g_build_filename(workspace, ".git", NULL);

        g_assert_true(g_file_test(dot_git, G_FILE_TEST_EXISTS));
    }

    g_assert_nonnull(detail);
    g_assert_nonnull(strstr(detail, "not a submodule"));
}

/*
 * Inside a repository it becomes a submodule, and the parent tracks it.
 *
 * git refuses file:// submodules by default (CVE-2022-39253), which is
 * a policy about local paths rather than anything clawtilla decides --
 * so the test relaxes it for its own child processes through
 * GIT_CONFIG_COUNT, which touches no configuration file anywhere. An
 * ordinary https: or ssh: URL is unaffected and needs no such thing.
 */
static void
test_a_git_import_inside_a_repository_becomes_a_submodule(void)
{
    g_auto(ImportScratch) scratch = import_scratch();
    g_autofree gchar *source = NULL;
    g_autofree gchar *state = NULL;
    g_autofree gchar *workspace = NULL;
    g_autofree gchar *detail = NULL;
    g_autofree gchar *modules = NULL;
    g_autofree gchar *contents = NULL;
    g_autoptr(GError) error = NULL;

    if (!have_git()) {
        g_test_skip("git is not installed");
        return;
    }

    source = g_build_filename(scratch, "source", NULL);
    state = g_build_filename(scratch, "state", NULL);
    workspace = g_build_filename(state, "agents", "scribe", NULL);

    g_assert_true(make_source_repo(source));
    g_assert_true(make_source_repo(state));

    g_setenv("GIT_CONFIG_COUNT", "1", TRUE);
    g_setenv("GIT_CONFIG_KEY_0", "protocol.file.allow", TRUE);
    g_setenv("GIT_CONFIG_VALUE_0", "always", TRUE);

    g_assert_true(clawt_workspace_adopt(CLAWT_IMPORT_GIT, source, workspace,
                                        FALSE, NULL, &detail, &error));

    g_unsetenv("GIT_CONFIG_COUNT");
    g_unsetenv("GIT_CONFIG_KEY_0");
    g_unsetenv("GIT_CONFIG_VALUE_0");

    g_assert_no_error(error);

    /*
     * The parent repository's own record of it. Without this the
     * workspace would be there and untracked, which is the outcome the
     * mode exists to avoid and is indistinguishable from a plain clone
     * by looking at the workspace alone.
     */
    modules = g_build_filename(state, ".gitmodules", NULL);
    g_assert_true(g_file_get_contents(modules, &contents, NULL, NULL));
    g_assert_nonnull(strstr(contents, "agents/scribe"));

    g_assert_nonnull(detail);
    g_assert_nonnull(strstr(detail, "submodule"));

    /* The path in the sentence is relative to the repository, not absolute. */
    g_assert_nonnull(strstr(detail, "agents/scribe"));
}

/*
 * A workspace already there is refused by the git mode too.
 *
 * The two directory modes are covered above; this one has its own
 * branch, and a clone into an occupied directory would fail somewhere
 * deep in git rather than saying what is wrong.
 */
static void
test_cloning_over_an_existing_workspace_is_refused(void)
{
    g_auto(ImportScratch) scratch = import_scratch();
    g_autofree gchar *workspace = NULL;
    g_autoptr(GError) error = NULL;

    workspace = g_build_filename(scratch, "occupied", NULL);
    g_assert_cmpint(g_mkdir_with_parents(workspace, 0700), ==, 0);

    g_assert_false(clawt_workspace_adopt(CLAWT_IMPORT_GIT,
                                         "https://example.invalid/x.git",
                                         workspace, FALSE, NULL, NULL,
                                         &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_ALREADY_EXISTS);
}

/*
 * git's own refusal reaches the caller.
 *
 * A URL that cannot be cloned is the ordinary failure here -- a typo, a
 * repository that is not there, a host that refuses -- and "import
 * failed" would send somebody to look at clawtilla. What they need is
 * what git said.
 */
static void
test_a_failed_clone_reports_what_git_said(void)
{
    g_auto(ImportScratch) scratch = import_scratch();
    g_autofree gchar *workspace = NULL;
    g_autoptr(GError) error = NULL;

    if (!have_git()) {
        g_test_skip("git is not installed");
        return;
    }

    workspace = g_build_filename(scratch, "doomed", NULL);

    g_assert_false(clawt_workspace_adopt(
        CLAWT_IMPORT_GIT, "/nonexistent/not-a-repo.git", workspace, FALSE,
        NULL, NULL, &error));
    g_assert_nonnull(error);
    g_assert_nonnull(strstr(error->message, "git refused"));
}

static guint
count_occurrences(const gchar *haystack, const gchar *needle)
{
    const gchar *p = haystack;
    guint n = 0;

    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p += strlen(needle);
    }

    return n;
}

/*
 * The computer region is rewritten in place, not appended to.
 *
 * replace_region() swaps everything from the begin marker to the end
 * marker inclusive, so a section that does not carry its own markers
 * removes them -- and the next start then finds no region and appends a
 * second copy, growing the file on every start. That is exactly what the
 * first version of this did, and reading the function did not show it;
 * reading the file after one start did.
 */
static void
test_the_computer_region_is_rewritten_not_appended(void)
{
    g_auto(ImportScratch) scratch = import_scratch();
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(GError) error = NULL;
    ClawtAgentConfig *agent;
    g_autofree gchar *yaml = NULL;
    g_autofree gchar *path = NULL;
    g_autofree gchar *contents = NULL;
    guint i;

    yaml = g_strdup_printf(
        "defaults:\n"
        "  workspace_root: '%s'\n"
        "agents:\n"
        "  - id: scribe\n"
        "    computer:\n"
        "      type: container\n", scratch);

    config = clawt_config_load_from_string(yaml, &error);
    g_assert_no_error(error);
    agent = clawt_config_get_agent(config, "scribe");

    g_assert_true(clawt_workspace_scaffold(agent, &error));
    g_assert_no_error(error);

    path = clawt_workspace_file_path(agent, "TOOLS.org");

    /*
     * Three times, because the failure is cumulative: the first write
     * looked perfect and the second was the one that doubled it.
     */
    for (i = 0; i < 3; i++) {
        g_autofree gchar *described =
            g_strdup_printf("You have a container. Shared: /srv = /work "
                            "(read-write). Round %u.", i);

        g_assert_true(clawt_workspace_update_computer(agent, described,
                                                      &error));
        g_assert_no_error(error);
    }

    g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));

    /* One region, and the markers still there to find it by. */
    g_assert_cmpint(
        count_occurrences(contents,
                          "# BEGIN clawtilla computer -- rewritten on every "
                          "start"), ==, 1);
    g_assert_cmpint(count_occurrences(contents, "# END clawtilla computer"),
                    ==, 1);
    g_assert_cmpint(count_occurrences(contents, "You have a container."),
                    ==, 1);

    /* And it holds the *latest* description, not the first. */
    g_assert_nonnull(strstr(contents, "Round 2."));
    g_assert_null(strstr(contents, "Round 0."));
}

/*
 * A description the agent can act on: both paths, and which is which.
 *
 * An agent's own read and write run on the host while its shell runs
 * inside the computer, so a shared folder has two names -- and telling
 * it only one is how it goes looking for a file at a path that does not
 * exist on the machine it is on. That is not hypothetical; it is
 * written down in this tree as a session somebody lost.
 */
static void
test_the_computer_region_names_both_sides(void)
{
    g_auto(ImportScratch) scratch = import_scratch();
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(GError) error = NULL;
    ClawtAgentConfig *agent;
    g_autofree gchar *yaml = NULL;
    g_autofree gchar *path = NULL;
    g_autofree gchar *contents = NULL;

    yaml = g_strdup_printf(
        "defaults:\n"
        "  workspace_root: '%s'\n"
        "agents:\n"
        "  - id: scribe\n"
        "    computer:\n"
        "      type: container\n", scratch);

    config = clawt_config_load_from_string(yaml, &error);
    g_assert_no_error(error);
    agent = clawt_config_get_agent(config, "scribe");

    g_assert_true(clawt_workspace_scaffold(agent, &error));
    g_assert_true(clawt_workspace_update_computer(
        agent, "Shared with the host, as host path = the path inside: "
               "/home/me/source = /work/source (read-write).", &error));

    path = clawt_workspace_file_path(agent, "TOOLS.org");
    g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));

    g_assert_nonnull(strstr(contents, "/home/me/source = /work/source"));

    /* And the sentence that says which side is which. */
    g_assert_nonnull(strstr(contents, "is on the host"));
    g_assert_nonnull(strstr(contents, "clawtilla_computer_exec"));

    /*
     * Said out loud, because a fleet default arrives without anything
     * about the agent changing -- so an agent re-reading its file has
     * no other way to know why a folder it did not have yesterday is
     * there today.
     */
    g_assert_nonnull(strstr(contents, "shared with every agent in the "
                                      "fleet"));
}

/*
 * ── What TOOLS.org says a connector may do ───────────────────────────
 *
 * A person can untick things on a consent screen, so the `scopes:` in
 * clawtilla.yaml is what was *asked for* and the token file is what was
 * given.  TOOLS.org used to print the ask under the sentence "Access was
 * granted for:", which is the wrong half of the pair -- and this is the
 * file an agent believes over any tool list, so it would confidently
 * spend turns on calls the service was always going to refuse and report
 * the refusals as the service misbehaving.
 */

/*
 * Puts a saved credential where a connector instance will find it, with
 * @granted as the scope set the provider issued, and points the instance
 * at it.  Returns the path so the caller can keep it alive.
 *
 * The token is built through clawt_oauth_token_parse() rather than by
 * writing the JSON by hand, so this fixture cannot drift from the file
 * format the daemon actually writes.
 */
static gchar *
connect_with_granted_scopes(Fixture *fixture, const gchar *name,
                            const gchar *granted)
{
    ClawtIntegrationConfig *instance =
        clawt_config_get_integration(fixture->config, name);
    g_autoptr(ClawtOauthToken) token = NULL;
    g_autofree gchar *json = NULL;
    g_autoptr(GError) error = NULL;
    gchar *path;

    g_assert_nonnull(instance);

    path = g_build_filename(fixture->dir, "connector-token.json", NULL);

    json = g_strdup_printf("{\"access_token\":\"secret-value\","
                           "\"token_type\":\"Bearer\","
                           "\"scope\":\"%s\","
                           "\"expires_in\":3600}", granted);

    token = clawt_oauth_token_parse(json, -1,
                                    g_get_real_time() / G_USEC_PER_SEC,
                                    &error);
    g_assert_no_error(error);
    g_assert_nonnull(token);

    g_assert_true(clawt_oauth_token_save(token, path, &error));
    g_assert_no_error(error);

    clawt_integration_config_set_string(instance, NULL, "token_file", path);

    return path;
}

static const gchar CONNECTOR_YAML[] =
    "integrations:\n"
    "  - name: forge\n"
    "    type: connector\n"
    "    scope: all\n"
    "    provider: github\n"
    "    scopes: \"repo,gist,delete_repo\"\n"
    "agents:\n"
    "  - id: scribe\n";

/*
 * The granted set is printed, and the requested one is not.  Asserting
 * on the absence of `delete_repo` is the whole point: a test that only
 * looked for `repo` would pass on either version, because the requested
 * string contains it.
 */
static void
test_a_connector_reports_the_granted_scopes(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    ClawtAgentConfig *agent;
    g_autofree gchar *token_path = NULL;
    g_autofree gchar *tools = NULL;

    fixture_setup(&fixture, CONNECTOR_YAML);
    agent = first_agent(&fixture);

    token_path = connect_with_granted_scopes(&fixture, "forge", "repo");

    g_assert_true(clawt_workspace_scaffold(agent, &error));
    g_assert_no_error(error);
    g_assert_true(clawt_workspace_update_tools_org(fixture.config, agent,
                                                   &error));
    g_assert_no_error(error);

    tools = read_workspace_file(agent, "TOOLS.org");
    g_assert_nonnull(tools);

    g_assert_nonnull(strstr(tools, "Access was granted for: repo."));
    g_assert_null(strstr(tools, "delete_repo"));
    g_assert_null(strstr(tools, "gist"));

    /*
     * And never the credential itself.  The granted set arrives in the
     * same struct as the access token, so the one thing this must not do
     * is copy the wrong field into a file the agent reads.
     */
    g_assert_null(strstr(tools, "secret-value"));

    fixture_teardown(&fixture);
}

/*
 * Nothing connected yet, so there is no granted set to report.  Saying
 * "granted" here would be the same lie about a different absence, so the
 * sentence says "requested" and that it may be narrower.
 */
static void
test_an_unconnected_connector_says_the_scopes_were_requested(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    ClawtAgentConfig *agent;
    g_autofree gchar *tools = NULL;

    fixture_setup(&fixture, CONNECTOR_YAML);
    agent = first_agent(&fixture);

    g_assert_true(clawt_workspace_scaffold(agent, &error));
    g_assert_true(clawt_workspace_update_tools_org(fixture.config, agent,
                                                   &error));
    g_assert_no_error(error);

    tools = read_workspace_file(agent, "TOOLS.org");
    g_assert_nonnull(tools);

    g_assert_nonnull(strstr(tools,
                            "Access was requested for: repo,gist,"
                            "delete_repo."));
    g_assert_null(strstr(tools, "Access was granted"));
    g_assert_nonnull(strstr(tools, "may be narrower"));

    fixture_teardown(&fixture);
}


/* ── What the persona costs ──────────────────────────────────────── */

/* Writes @bytes of filler into a workspace file, plus a newline. */
static void
write_identity_file(const gchar *workspace, const gchar *name, gsize bytes)
{
    g_autofree gchar *path = g_build_filename(workspace, name, NULL);
    g_autofree gchar *filler = g_strnfill(bytes, 'x');

    g_assert_cmpint(g_mkdir_with_parents(workspace, 0700), ==, 0);
    g_assert_true(g_file_set_contents(path, filler, (gssize)bytes, NULL));
}

static ClawtIdentityFile *
find_identity_file(ClawtIdentitySize *size, const gchar *name)
{
    guint i;

    for (i = 0; i < size->files->len; i++) {
        ClawtIdentityFile *file = g_ptr_array_index(size->files, i);

        if (g_strcmp0(file->name, name) == 0)
            return file;
    }

    return NULL;
}

/*
 * The arithmetic is libreclaw's, checked against libreclaw.
 *
 * A count of our own that merely *looks* like the assembly would be
 * wrong by the header on every file and nobody would notice, because the
 * only thing that ever compares them is the kernel -- once, at the
 * cliff, with an error naming neither.  clawtilla links liblc, so the
 * test builds the same prompt through the same function and asserts the
 * byte counts are equal rather than close.
 */
static void
test_the_measurement_matches_libreclaws_own_assembly(void)
{
    Fixture fixture;
    g_autoptr(LcAgentContext) context = NULL;
    g_autoptr(ClawtIdentitySize) size = NULL;
    g_autofree gchar *workspace = NULL;
    g_auto(GStrv) names = NULL;
    ClawtAgentConfig *agent;

    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: chief\n"
                  "    persona:\n"
                  "      identity_files: [SOUL.org, PROJECTS.org]\n");

    agent = first_agent(&fixture);
    workspace = clawt_agent_config_get_workspace(agent);

    write_identity_file(workspace, "SOUL.org", 900);
    write_identity_file(workspace, "PROJECTS.org", 4321);

    size = clawt_workspace_measure_identity(agent);

    names = clawt_workspace_effective_identity_files(agent);
    context = lc_agent_context_new("chief", workspace);
    g_assert_true(lc_agent_context_load_identity(
        context, (const gchar * const *)names, NULL));

    g_assert_cmpuint(size->total, ==,
                     strlen(lc_agent_context_get_system_prompt(context)));

    fixture_teardown(&fixture);
}

/*
 * A file with an embedded NUL costs less than its size on disk.
 *
 * The assembly is a printf, which stops at the first NUL, so a count
 * taken from the file's length would say the prompt is larger than it is
 * -- wrong in the direction that makes a working agent look doomed.
 * This is the case a count using g_file_get_contents()'s length would
 * pass every other test and fail only here.
 */
static void
test_an_embedded_nul_stops_the_count(void)
{
    Fixture fixture;
    g_autoptr(LcAgentContext) context = NULL;
    g_autoptr(ClawtIdentitySize) size = NULL;
    g_autofree gchar *workspace = NULL;
    g_autofree gchar *path = NULL;
    g_auto(GStrv) names = NULL;
    ClawtAgentConfig *agent;
    ClawtIdentityFile *soul;
    gchar payload[64];

    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: chief\n"
                  "    persona:\n"
                  "      identity_files: [SOUL.org]\n");

    agent = first_agent(&fixture);
    workspace = clawt_agent_config_get_workspace(agent);

    g_assert_cmpint(g_mkdir_with_parents(workspace, 0700), ==, 0);
    memset(payload, 'y', sizeof payload);
    payload[10] = '\0';

    path = g_build_filename(workspace, "SOUL.org", NULL);
    g_assert_true(g_file_set_contents(path, payload, sizeof payload, NULL));

    size = clawt_workspace_measure_identity(agent);
    soul = find_identity_file(size, "SOUL.org");

    g_assert_nonnull(soul);
    g_assert_true(soul->present);

    /* "# " + "SOUL.org" + "\n\n" + 10 bytes + "\n\n" */
    g_assert_cmpuint(soul->bytes, ==, 2 + 8 + 2 + 10 + 2);

    names = clawt_workspace_effective_identity_files(agent);
    context = lc_agent_context_new("chief", workspace);
    g_assert_true(lc_agent_context_load_identity(
        context, (const gchar * const *)names, NULL));
    g_assert_cmpuint(size->total, ==,
                     strlen(lc_agent_context_get_system_prompt(context)));

    fixture_teardown(&fixture);
}

/*
 * A named file the workspace does not have is listed at zero rather than
 * dropped.  libreclaw warns and carries on for the same reason, and a
 * breakdown that quietly omitted the name would send somebody looking for
 * a file that is not there.
 */
static void
test_a_missing_identity_file_is_listed_at_zero(void)
{
    Fixture fixture;
    g_autoptr(ClawtIdentitySize) size = NULL;
    g_autofree gchar *workspace = NULL;
    ClawtAgentConfig *agent;
    ClawtIdentityFile *missing;

    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: chief\n"
                  "    persona:\n"
                  "      identity_files: [SOUL.org, GONE.org]\n");

    agent = first_agent(&fixture);
    workspace = clawt_agent_config_get_workspace(agent);
    write_identity_file(workspace, "SOUL.org", 100);

    size = clawt_workspace_measure_identity(agent);

    g_assert_cmpuint(size->files->len, ==, 2);
    g_assert_cmpuint(size->present, ==, 1);

    missing = find_identity_file(size, "GONE.org");
    g_assert_nonnull(missing);
    g_assert_false(missing->present);
    g_assert_cmpuint(missing->bytes, ==, 0);

    fixture_teardown(&fixture);
}

/*
 * Nothing is said about an agent that is nowhere near the limit.
 *
 * A byte count reported on every agent is noise, and noise is exactly
 * what stops the one that matters from being read.
 */
static void
test_a_small_persona_gets_no_verdict(void)
{
    Fixture fixture;
    g_autoptr(ClawtIdentitySize) size = NULL;
    g_autofree gchar *workspace = NULL;
    g_autofree gchar *verdict = NULL;
    ClawtAgentConfig *agent;

    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: chief\n"
                  "    persona:\n"
                  "      identity_files: [SOUL.org]\n");

    agent = first_agent(&fixture);
    workspace = clawt_agent_config_get_workspace(agent);
    write_identity_file(workspace, "SOUL.org", 4096);

    size = clawt_workspace_measure_identity(agent);
    verdict = clawt_workspace_identity_verdict(size);

    g_assert_null(verdict);

    fixture_teardown(&fixture);
}

/*
 * The threshold is a boundary, so it is tested as one.  Just under says
 * nothing; at it, the verdict appears.
 */
static void
test_the_notice_threshold_is_a_boundary(void)
{
    Fixture fixture;
    g_autofree gchar *workspace = NULL;
    ClawtAgentConfig *agent;
    gsize notice = clawt_identity_notice_bytes(CLAWT_ARG_LIMIT);
    gsize header = 2 + strlen("SOUL.org") + 2 + 2;

    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: chief\n"
                  "    persona:\n"
                  "      identity_files: [SOUL.org]\n");

    agent = first_agent(&fixture);
    workspace = clawt_agent_config_get_workspace(agent);

    {
        g_autoptr(ClawtIdentitySize) size = NULL;
        g_autofree gchar *verdict = NULL;

        write_identity_file(workspace, "SOUL.org", notice - header - 1);
        size = clawt_workspace_measure_identity(agent);
        g_assert_cmpuint(size->total, ==, notice - 1);

        verdict = clawt_workspace_identity_verdict(size);
        g_assert_null(verdict);
    }

    {
        g_autoptr(ClawtIdentitySize) size = NULL;
        g_autofree gchar *verdict = NULL;

        write_identity_file(workspace, "SOUL.org", notice - header);
        size = clawt_workspace_measure_identity(agent);
        g_assert_cmpuint(size->total, ==, notice);

        verdict = clawt_workspace_identity_verdict(size);
        g_assert_nonnull(verdict);

        /* Approaching, not over: the two need different actions. */
        g_assert_null(strstr(verdict, "over the"));
        g_assert_nonnull(strstr(verdict, "SOUL.org"));
    }

    fixture_teardown(&fixture);
}

/*
 * Past the limit the sentence changes, and it says why nothing has
 * failed yet: a resumed session is never handed a system prompt, so the
 * agent goes on working until something starts a fresh one.  Somebody
 * told only "too large" would look for a break that is not there.
 */
static void
test_over_the_limit_says_what_still_works(void)
{
    Fixture fixture;
    g_autoptr(ClawtIdentitySize) size = NULL;
    g_autofree gchar *workspace = NULL;
    g_autofree gchar *verdict = NULL;
    ClawtAgentConfig *agent;

    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: chief\n"
                  "    persona:\n"
                  "      identity_files: [SOUL.org, PROJECTS.org]\n");

    agent = first_agent(&fixture);
    workspace = clawt_agent_config_get_workspace(agent);

    write_identity_file(workspace, "SOUL.org", 4096);
    write_identity_file(workspace, "PROJECTS.org", CLAWT_ARG_LIMIT);

    size = clawt_workspace_measure_identity(agent);
    g_assert_cmpuint(size->total, >=, CLAWT_ARG_LIMIT);

    verdict = clawt_workspace_identity_verdict(size);
    g_assert_nonnull(verdict);
    g_assert_nonnull(strstr(verdict, "over the"));
    g_assert_nonnull(strstr(verdict, "resumed session keeps working"));

    /*
     * Largest first, because the file to shorten is the answer somebody
     * actually wants -- and PROJECTS.org is the one the scaffolding tells
     * the agent to keep growing.
     */
    g_assert_nonnull(strstr(verdict, "PROJECTS.org"));
    g_assert_cmpint(strstr(verdict, "PROJECTS.org") - verdict, <,
                    strstr(verdict, "SOUL.org") - verdict);

    fixture_teardown(&fixture);
}

/*
 * An agent with an inline persona has no identity files, so there is
 * nothing to measure and nothing to say.  It must not be reported as an
 * agent with an empty identity, which is a different and alarming thing.
 */
static void
test_an_inline_persona_measures_nothing(void)
{
    Fixture fixture;
    g_autoptr(ClawtIdentitySize) size = NULL;
    g_autofree gchar *verdict = NULL;

    fixture_setup(&fixture,
                  "agents:\n"
                  "  - id: chief\n"
                  "    persona:\n"
                  "      system_prompt: \"you are brief\"\n");

    size = clawt_workspace_measure_identity(first_agent(&fixture));

    g_assert_cmpuint(size->files->len, ==, 0);
    g_assert_cmpuint(size->total, ==, 0);

    verdict = clawt_workspace_identity_verdict(size);
    g_assert_null(verdict);

    fixture_teardown(&fixture);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/workspace/agent-maintains-its-own-files",
                    test_the_agent_is_told_to_maintain_its_own_files);
    g_test_add_func("/workspace/scaffold-writes-the-set",
                    test_scaffold_writes_the_standard_set);
    g_test_add_func("/workspace/scaffold-never-overwrites",
                    test_scaffold_never_overwrites);
    g_test_add_func("/workspace/ask-agent-is-not-promised-to-block",
                    test_scaffold_does_not_promise_ask_agent_blocks);
    g_test_add_func("/workspace/describes-this-agent",
                    test_scaffold_describes_this_agent);
    g_test_add_func("/workspace/no-computer-is-said-out-loud",
                    test_scaffold_says_when_there_is_no_computer);
    g_test_add_func("/workspace/mcp-config", test_mcp_config_is_written);
    g_test_add_func("/workspace/mcp-config-guest-desktop",
                    test_mcp_config_offers_a_guest_desktop);
    g_test_add_func("/workspace/mcp-config-no-desktop-without-a-vm",
                    test_mcp_config_has_no_desktop_without_a_vm);
    g_test_add_func("/workspace/mcp-config-drops-a-revoked-desktop",
                    test_mcp_config_drops_a_desktop_that_was_revoked);
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
    g_test_add_func("/workspace/skills-come-from-the-workspace",
                    test_skills_come_from_the_workspace);
    g_test_add_func("/workspace/string-list-round-trips",
                    test_string_list_round_trips);
    g_test_add_func("/workspace/scaffold-skips-unloaded-identity",
                    test_scaffold_skips_unloaded_identity_files);
    g_test_add_func("/workspace/detects-markdown-persona",
                    test_detects_an_existing_markdown_persona);
    g_test_add_func("/workspace/detects-nothing-when-org-present",
                    test_detects_nothing_when_org_is_present);
    g_test_add_func("/workspace/detects-nothing-when-empty",
                    test_detects_nothing_in_an_empty_workspace);
    g_test_add_func("/workspace/every-agent-list-round-trips",
                    test_every_agent_list_round_trips);
    g_test_add_func("/workspace/clearing-a-list-unsets-it",
                    test_clearing_a_list_unsets_it);
    g_test_add_func("/workspace/a-lone-scalar-is-a-one-element-list",
                    test_a_lone_scalar_is_a_one_element_list);
    g_test_add_func("/workspace/an-empty-scalar-is-still-unset",
                    test_an_empty_scalar_is_still_unset);
    g_test_add_func("/workspace/set-from-string-follows-the-schema",
                    test_set_from_string_follows_the_schema);
    g_test_add_func("/workspace/purpose-becomes-the-mission",
                    test_scaffold_writes_the_purpose_as_the_mission);
    g_test_add_func("/workspace/no-mission-keeps-the-nudge",
                    test_scaffold_without_a_mission_keeps_the_nudge);
    g_test_add_func("/workspace/mission-that-did-not-land-is-reported",
                    test_scaffold_says_when_the_mission_did_not_land);
    g_test_add_func("/workspace/scaffold-and-renderer-agree",
                    test_scaffold_and_renderer_agree);
    g_test_add_func("/workspace/computer-region-is-rewritten",
                    test_the_computer_region_is_rewritten_not_appended);
    g_test_add_func("/workspace/computer-region-names-both-sides",
                    test_the_computer_region_names_both_sides);
    g_test_add_func("/workspace/connector-reports-granted-scopes",
                    test_a_connector_reports_the_granted_scopes);
    g_test_add_func("/workspace/connector-says-requested-when-unconnected",
                    test_an_unconnected_connector_says_the_scopes_were_requested);
    g_test_add_func("/import/link-points-at-the-original",
                    test_a_linked_workspace_points_at_the_original);
    g_test_add_func("/import/link-over-existing-is-refused",
                    test_linking_over_an_existing_workspace_is_refused);
    g_test_add_func("/import/removing-a-link-leaves-the-original",
                    test_removing_a_linked_workspace_leaves_the_original);
    g_test_add_func("/import/a-copy-says-it-is-separate",
                    test_a_copy_says_the_original_is_separate);
    g_test_add_func("/import/missing-source-is-refused",
                    test_a_missing_source_is_refused_by_both_directory_modes);
    g_test_add_func("/import/modes-round-trip",
                    test_every_import_mode_round_trips);
    g_test_add_func("/workspace/identity-size-matches-libreclaw",
                    test_the_measurement_matches_libreclaws_own_assembly);
    g_test_add_func("/workspace/identity-size-stops-at-an-embedded-nul",
                    test_an_embedded_nul_stops_the_count);
    g_test_add_func("/workspace/identity-size-lists-a-missing-file",
                    test_a_missing_identity_file_is_listed_at_zero);
    g_test_add_func("/workspace/identity-verdict-is-silent-when-small",
                    test_a_small_persona_gets_no_verdict);
    g_test_add_func("/workspace/identity-verdict-threshold-is-a-boundary",
                    test_the_notice_threshold_is_a_boundary);
    g_test_add_func("/workspace/identity-verdict-over-the-limit",
                    test_over_the_limit_says_what_still_works);
    g_test_add_func("/workspace/identity-size-of-an-inline-persona",
                    test_an_inline_persona_measures_nothing);
    g_test_add_func("/import/git-outside-a-repository-clones",
                    test_a_git_import_outside_a_repository_clones);
    g_test_add_func("/import/git-inside-a-repository-submodules",
                    test_a_git_import_inside_a_repository_becomes_a_submodule);
    g_test_add_func("/import/clone-over-existing-is-refused",
                    test_cloning_over_an_existing_workspace_is_refused);
    g_test_add_func("/import/failed-clone-reports-git",
                    test_a_failed_clone_reports_what_git_said);
    g_test_add_func("/import/submodule-decision-follows-the-repository",
                    test_the_submodule_decision_follows_the_repository);

    return g_test_run();
}
