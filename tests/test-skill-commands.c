/*
 * test-skill-commands.c - `/name` in clawtilla's own chat
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Expansion happens daemon-side so that both clients send identical
 * text, which means these tests are the *only* place either client's
 * behaviour is checked -- a bug here is a bug in both of them at once.
 *
 * The `%s` test is not a formality.  A skill body is a file somebody
 * else wrote, and this project has an explicit rule that such a string
 * never reaches printf; the assertion is that a percent and an ess come
 * out the other side as a percent and an ess.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>
#include <string.h>

#include "clawt-test-util.h"

typedef struct {
    gchar             *dir;
    gchar             *skills_dir;
    ClawtConfig       *config;
    ClawtSkillLibrary *library;
} Fixture;

static void
fixture_setup_for(Fixture *fixture, const gchar *provider)
{
    g_autofree gchar *yaml = NULL;
    g_autoptr(GError) error = NULL;

    fixture->dir = g_dir_make_tmp("clawt-cmd-XXXXXX", NULL);
    fixture->skills_dir = g_build_filename(fixture->dir, "skills", NULL);
    g_assert_true(clawt_ensure_dir(fixture->skills_dir, 0700, NULL));

    yaml = g_strdup_printf(
        "daemon:\n"
        "  state_dir: \"%s/state\"\n"
        "  socket: \"%s/sock\"\n"
        "  automation_dir: \"%s/pods\"\n"
        "  tailscale: false\n"
        "defaults:\n"
        "  workspace_root: \"%s/agents\"\n"
        "  skills: [release]\n"
        "skills:\n"
        "  dir: \"%s\"\n"
        "agents:\n"
        "  - id: builder\n"
        "    model:\n"
        "      provider: %s\n",
        fixture->dir, fixture->dir, fixture->dir, fixture->dir,
        fixture->skills_dir, provider);

    fixture->config = clawt_config_load_from_string(yaml, &error);
    g_assert_no_error(error);

    fixture->library = clawt_skill_library_new(fixture->skills_dir);
}

static void
fixture_setup(Fixture *fixture)
{
    fixture_setup_for(fixture, "claude-code");
}

static void
fixture_teardown(Fixture *fixture)
{
    g_clear_object(&fixture->library);
    g_clear_object(&fixture->config);

    if (fixture->dir != NULL)
        clawt_test_remove_tree(fixture->dir);

    g_clear_pointer(&fixture->skills_dir, g_free);
    g_clear_pointer(&fixture->dir, g_free);
}

static ClawtAgentConfig *
builder(Fixture *fixture)
{
    ClawtAgentConfig *agent = clawt_config_get_agent(fixture->config,
                                                     "builder");

    g_assert_nonnull(agent);

    return agent;
}

/* Write a skill, then link it into the agent's workspace. */
static void
give_skill(Fixture *fixture, const gchar *name, const gchar *body)
{
    g_autofree gchar *dir = g_build_filename(fixture->skills_dir, name, NULL);
    g_autofree gchar *file = g_build_filename(dir, "SKILL.md", NULL);
    g_autofree gchar *text = NULL;
    g_autoptr(GPtrArray) warnings = NULL;
    g_autoptr(GError) error = NULL;
    g_auto(GStrv) names = NULL;

    g_assert_true(clawt_ensure_dir(dir, 0700, NULL));
    text = g_strdup_printf("---\nname: %s\ndescription: Cut a %s\n---\n\n%s\n",
                           name, name,
                           body != NULL ? body : "Do the thing.");
    g_assert_true(g_file_set_contents(file, text, -1, NULL));

    clawt_skill_library_scan(fixture->library);

    names = g_strsplit(name, ",", -1);
    g_assert_true(clawt_config_set_string_list(
        fixture->config, "defaults.skills",
        (const gchar *const *)names));

    g_assert_true(clawt_skill_provision(fixture->config, builder(fixture),
                                        fixture->library, &warnings, &error));
    g_assert_no_error(error);
}

/*
 * A command file somebody wrote by hand.
 *
 * Used wherever the test needs to control the exact body, because a
 * command clawtilla generated for a skill is deliberately a *pointer*
 * to that skill rather than a copy of it -- see the precedence test.
 */
static void
write_command(Fixture *fixture, const gchar *name, const gchar *body)
{
    g_autofree gchar *workspace =
        clawt_agent_config_get_workspace(builder(fixture));
    g_autofree gchar *dir = g_build_filename(workspace, ".claude/commands",
                                             NULL);
    g_autofree gchar *path = NULL;
    g_autofree gchar *text = NULL;

    g_assert_true(clawt_ensure_dir(dir, 0700, NULL));

    path = g_build_filename(dir, name, NULL);
    text = g_strdup_printf("---\ndescription: %s\n---\n%s\n", name, body);

    g_assert_true(g_file_set_contents(path, text, -1, NULL));
}

static ClawtSkillCommand *
command_named(GPtrArray *commands, const gchar *name)
{
    guint i;

    for (i = 0; i < commands->len; i++) {
        ClawtSkillCommand *command = g_ptr_array_index(commands, i);

        if (g_strcmp0(command->name, name) == 0)
            return command;
    }

    return NULL;
}

static void
test_a_linked_skill_becomes_a_command(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GPtrArray) commands = NULL;
    ClawtSkillCommand *release;

    fixture_setup(&fixture);
    give_skill(&fixture, "release", NULL);

    commands = clawt_skill_commands_list(builder(&fixture));
    release = command_named(commands, "release");

    g_assert_nonnull(release);
    g_assert_cmpstr(release->description, ==, "Cut a release");

    fixture_teardown(&fixture);
}

static void
test_an_argument_hint_is_reported(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *workspace = NULL;
    g_autofree gchar *dir = NULL;
    g_autofree gchar *path = NULL;
    g_autoptr(GPtrArray) commands = NULL;
    ClawtSkillCommand *command;

    fixture_setup(&fixture);

    /*
     * Through a hand-written command file rather than a skill, because
     * that is where an argument hint is normally written -- and because
     * it proves the listing reads the *workspace* rather than
     * clawtilla's own bindings.  A command somebody wrote by hand
     * genuinely works, and a listing that only knew about our links
     * would offer fewer commands than the CLI has.
     */
    workspace = clawt_agent_config_get_workspace(builder(&fixture));
    dir = g_build_filename(workspace, ".claude/commands", NULL);
    g_assert_true(clawt_ensure_dir(dir, 0700, NULL));

    path = g_build_filename(dir, "ship.md", NULL);
    g_assert_true(g_file_set_contents(
        path,
        "---\ndescription: Ship it\nargument-hint: \"[version]\"\n---\n"
        "Ship $ARGUMENTS.\n", -1, NULL));

    commands = clawt_skill_commands_list(builder(&fixture));
    command = command_named(commands, "ship");

    g_assert_nonnull(command);
    g_assert_cmpstr(command->description, ==, "Ship it");
    g_assert_cmpstr(command->argument_hint, ==, "[version]");

    fixture_teardown(&fixture);
}

static void
test_expansion_substitutes_the_arguments(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *prompt = NULL;
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture);
    write_command(&fixture, "ship.md", "Cut $ARGUMENTS and tell me.");

    prompt = clawt_skill_commands_expand(builder(&fixture), "ship",
                                         "v1.2.0", &error);

    g_assert_no_error(error);
    g_assert_nonnull(strstr(prompt, "Cut v1.2.0 and tell me."));

    fixture_teardown(&fixture);
}

static void
test_expansion_with_no_arguments_leaves_nothing_behind(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *prompt = NULL;
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture);
    write_command(&fixture, "ship.md", "Cut $ARGUMENTS and tell me.");

    prompt = clawt_skill_commands_expand(builder(&fixture), "ship", NULL,
                                         &error);

    g_assert_no_error(error);

    /*
     * The placeholder is consumed rather than left as literal text: a
     * model handed "Cut $ARGUMENTS" asks what $ARGUMENTS is.
     */
    g_assert_null(strstr(prompt, "$ARGUMENTS"));

    fixture_teardown(&fixture);
}

/*
 * A command file wins over a skill of the same name, and for a skill
 * clawtilla linked that file is a *pointer* rather than a copy.
 *
 * That is the right answer and it is worth pinning, because the obvious
 * alternative -- pasting the skill's body into the message -- bypasses
 * the harness's own skill mechanism and puts the whole procedure into
 * the message rather than into the place the CLI loads it from.
 */
static void
test_a_generated_command_points_at_the_skill(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *prompt = NULL;
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture);
    give_skill(&fixture, "release", "Step one. Step two. Step three.");

    prompt = clawt_skill_commands_expand(builder(&fixture), "release", NULL,
                                         &error);

    g_assert_no_error(error);
    g_assert_nonnull(strstr(prompt, "release"));
    g_assert_nonnull(strstr(prompt, "SKILL.md"));
    g_assert_null(strstr(prompt, "Step one."));

    fixture_teardown(&fixture);
}

/*
 * On a harness with no commands directory -- grok, whose entire slash
 * mechanism *is* skills -- the skill itself is what a slash resolves to.
 *
 * So `/name` works on every provider, and it is the registry rather than
 * clawtilla that decides how.
 */
static void
test_a_skill_expands_where_there_is_no_command_file(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *prompt = NULL;
    g_autoptr(GError) error = NULL;

    fixture_setup_for(&fixture, "grok-build");
    give_skill(&fixture, "release", "Cut $ARGUMENTS and tell me.");

    prompt = clawt_skill_commands_expand(builder(&fixture), "release",
                                         "v1.2.0", &error);

    g_assert_no_error(error);
    g_assert_nonnull(strstr(prompt, "Cut v1.2.0 and tell me."));

    fixture_teardown(&fixture);
}

static void
test_a_leading_slash_is_accepted(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *with = NULL;
    g_autofree gchar *without = NULL;
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture);
    give_skill(&fixture, "release", "Cut it.");

    with = clawt_skill_commands_expand(builder(&fixture), "/release", NULL,
                                       &error);
    g_assert_no_error(error);

    without = clawt_skill_commands_expand(builder(&fixture), "release", NULL,
                                          &error);
    g_assert_no_error(error);

    g_assert_cmpstr(with, ==, without);

    fixture_teardown(&fixture);
}

/*
 * A skill body is a file somebody else wrote.  A `%s` in it is a percent
 * and an ess, and it must never be handed to printf -- which is an
 * explicit rule in this codebase and the kind that is broken by a
 * convenience wrapper somewhere far from here.
 */
static void
test_a_percent_s_in_a_body_is_literal(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *prompt = NULL;
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture);
    write_command(&fixture, "ship.md",
                  "Run `printf %s %d %n` and report what it said.");

    prompt = clawt_skill_commands_expand(builder(&fixture), "ship", NULL,
                                         &error);

    g_assert_no_error(error);
    g_assert_nonnull(strstr(prompt, "printf %s %d %n"));

    fixture_teardown(&fixture);
}

static void
test_an_unknown_command_is_refused_by_name(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *prompt = NULL;
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture);
    give_skill(&fixture, "release", "Cut it.");

    prompt = clawt_skill_commands_expand(builder(&fixture), "nonesuch", NULL,
                                         &error);

    g_assert_null(prompt);
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND);

    fixture_teardown(&fixture);
}

/*
 * Two harnesses can put a command of the same name in one workspace --
 * ai-glib's own directory and claude's, say.  The set resolves the
 * collision to one winner; the listing must offer it once, or a
 * completion popup shows the same `/name` twice.
 */
static void
test_a_name_collision_is_offered_once(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *workspace = NULL;
    g_autofree gchar *claude_dir = NULL;
    g_autofree gchar *cursor_dir = NULL;
    g_autofree gchar *a = NULL;
    g_autofree gchar *b = NULL;
    g_autoptr(GPtrArray) commands = NULL;
    guint seen = 0;
    guint i;

    fixture_setup(&fixture);
    workspace = clawt_agent_config_get_workspace(builder(&fixture));

    claude_dir = g_build_filename(workspace, ".claude/commands", NULL);
    cursor_dir = g_build_filename(workspace, ".cursor/commands", NULL);
    g_assert_true(clawt_ensure_dir(claude_dir, 0700, NULL));
    g_assert_true(clawt_ensure_dir(cursor_dir, 0700, NULL));

    a = g_build_filename(claude_dir, "ship.md", NULL);
    b = g_build_filename(cursor_dir, "ship.md", NULL);
    g_assert_true(g_file_set_contents(
        a, "---\ndescription: from claude\n---\nA\n", -1, NULL));
    g_assert_true(g_file_set_contents(
        b, "---\ndescription: from cursor\n---\nB\n", -1, NULL));

    commands = clawt_skill_commands_list(builder(&fixture));

    for (i = 0; i < commands->len; i++) {
        ClawtSkillCommand *command = g_ptr_array_index(commands, i);

        if (g_strcmp0(command->name, "ship") == 0)
            seen++;
    }

    g_assert_cmpuint(seen, ==, 1);

    fixture_teardown(&fixture);
}

/*
 * A namespaced resource -- claude spells `git/status.md` as
 * `/git:status` -- is not offered.
 *
 * The name goes back over IPC and is looked up again, and a colon is
 * not in the gate this subsystem validates names against.  Offering it
 * would mean a completion popup with an entry that cannot be invoked.
 */
static void
test_a_name_that_is_not_a_command_is_skipped(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *workspace = NULL;
    g_autofree gchar *nested = NULL;
    g_autofree gchar *path = NULL;
    g_autoptr(GPtrArray) commands = NULL;
    guint i;

    fixture_setup(&fixture);
    workspace = clawt_agent_config_get_workspace(builder(&fixture));

    nested = g_build_filename(workspace, ".claude/commands/git", NULL);
    g_assert_true(clawt_ensure_dir(nested, 0700, NULL));

    path = g_build_filename(nested, "status.md", NULL);
    g_assert_true(g_file_set_contents(
        path, "---\ndescription: git status\n---\nS\n", -1, NULL));

    commands = clawt_skill_commands_list(builder(&fixture));

    for (i = 0; i < commands->len; i++) {
        ClawtSkillCommand *command = g_ptr_array_index(commands, i);

        g_assert_null(strchr(command->name, ':'));
    }

    fixture_teardown(&fixture);
}

/*
 * The shell-substitution path is closed.
 *
 * ai-glib's default policy is opt-in: a resource declaring `shell: true`
 * has its `` !`cmd` `` executed at resolve time.  Here that would be a
 * subprocess started from an IPC handler on the daemon's own context --
 * and a file dropped into a skills directory deciding to run code on the
 * daemon's host, which is not a permission anyone granted by assigning a
 * skill.
 */
static void
test_a_shell_substitution_is_never_run(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *workspace = NULL;
    g_autofree gchar *dir = NULL;
    g_autofree gchar *path = NULL;
    g_autofree gchar *witness = NULL;
    g_autofree gchar *body = NULL;
    g_autofree gchar *prompt = NULL;
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture);
    workspace = clawt_agent_config_get_workspace(builder(&fixture));

    dir = g_build_filename(workspace, ".claude/commands", NULL);
    g_assert_true(clawt_ensure_dir(dir, 0700, NULL));

    /*
     * The assertion is on a file the command would create, not on the
     * text: a substitution that ran and produced nothing would leave a
     * prompt that looks exactly like one that never ran.
     */
    witness = g_build_filename(fixture.dir, "it-ran", NULL);
    body = g_strdup_printf(
        "---\ndescription: d\nshell: true\n---\n"
        "Output was: !`touch %s`\n", witness);

    path = g_build_filename(dir, "sneaky.md", NULL);
    g_assert_true(g_file_set_contents(path, body, -1, NULL));

    prompt = clawt_skill_commands_expand(builder(&fixture), "sneaky", NULL,
                                         &error);

    g_assert_no_error(error);
    g_assert_nonnull(prompt);
    g_assert_false(g_file_test(witness, G_FILE_TEST_EXISTS));

    fixture_teardown(&fixture);
}

static void
test_a_command_line_is_recognised(void)
{
    g_assert_true(clawt_skill_command_line_is_command("/release"));
    g_assert_true(clawt_skill_command_line_is_command("/release v1"));
    g_assert_false(clawt_skill_command_line_is_command("release"));
    g_assert_false(clawt_skill_command_line_is_command(NULL));
    g_assert_false(clawt_skill_command_line_is_command(""));
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/skill-commands/listed",
                    test_a_linked_skill_becomes_a_command);
    g_test_add_func("/skill-commands/argument-hint",
                    test_an_argument_hint_is_reported);
    g_test_add_func("/skill-commands/expand",
                    test_expansion_substitutes_the_arguments);
    g_test_add_func("/skill-commands/expand-empty",
                    test_expansion_with_no_arguments_leaves_nothing_behind);
    g_test_add_func("/skill-commands/generated-points-at-skill",
                    test_a_generated_command_points_at_the_skill);
    g_test_add_func("/skill-commands/skill-without-command-file",
                    test_a_skill_expands_where_there_is_no_command_file);
    g_test_add_func("/skill-commands/leading-slash",
                    test_a_leading_slash_is_accepted);
    g_test_add_func("/skill-commands/percent-s",
                    test_a_percent_s_in_a_body_is_literal);
    g_test_add_func("/skill-commands/unknown",
                    test_an_unknown_command_is_refused_by_name);
    g_test_add_func("/skill-commands/collision",
                    test_a_name_collision_is_offered_once);
    g_test_add_func("/skill-commands/namespaced-skipped",
                    test_a_name_that_is_not_a_command_is_skipped);
    g_test_add_func("/skill-commands/no-shell",
                    test_a_shell_substitution_is_never_run);
    g_test_add_func("/skill-commands/is-command-line",
                    test_a_command_line_is_recognised);

    return g_test_run();
}
