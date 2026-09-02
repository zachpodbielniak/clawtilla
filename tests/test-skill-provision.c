/*
 * test-skill-provision.c - Links, at the paths each harness really reads
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The paths are asked of ai-glib's registry rather than written down,
 * and these tests assert on the *result* of that -- which is the only
 * way to notice if the question stops being asked.  They also pin the
 * two facts that are counterintuitive enough to be worth a test each:
 * grok has no commands directory at all, and antigravity gets a JSON
 * manifest rather than a symlink.
 *
 * The ownership tests are the ones that would otherwise be found by
 * losing somebody's work: clawtilla removes only the links it made, and
 * a real directory sitting at one of these paths is left exactly as it
 * is.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

#include "clawt-test-util.h"

typedef struct {
    gchar             *dir;
    gchar             *skills_dir;
    ClawtConfig       *config;
    ClawtSkillLibrary *library;
} Fixture;

static void
fixture_setup(Fixture *fixture, const gchar *provider)
{
    g_autofree gchar *yaml = NULL;
    g_autoptr(GError) error = NULL;

    fixture->dir = g_dir_make_tmp("clawt-prov-XXXXXX", NULL);
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
fixture_teardown(Fixture *fixture)
{
    g_clear_object(&fixture->library);
    g_clear_object(&fixture->config);

    if (fixture->dir != NULL)
        clawt_test_remove_tree(fixture->dir);

    g_clear_pointer(&fixture->skills_dir, g_free);
    g_clear_pointer(&fixture->dir, g_free);
}

static void
write_skill(Fixture *fixture, const gchar *name)
{
    g_autofree gchar *dir = g_build_filename(fixture->skills_dir, name, NULL);
    g_autofree gchar *file = g_build_filename(dir, "SKILL.md", NULL);
    g_autofree gchar *text = NULL;

    g_assert_true(clawt_ensure_dir(dir, 0700, NULL));
    text = g_strdup_printf("---\nname: %s\ndescription: Cut a %s\n---\n\n"
                           "Do it.\n", name, name);
    g_assert_true(g_file_set_contents(file, text, -1, NULL));

    clawt_skill_library_scan(fixture->library);
}

static ClawtAgentConfig *
builder(Fixture *fixture)
{
    ClawtAgentConfig *agent = clawt_config_get_agent(fixture->config,
                                                     "builder");

    g_assert_nonnull(agent);

    return agent;
}

static gboolean
provision(Fixture *fixture, GPtrArray **warnings)
{
    g_autoptr(GError) error = NULL;
    gboolean ok = clawt_skill_provision(fixture->config, builder(fixture),
                                        fixture->library, warnings, &error);

    g_assert_no_error(error);

    return ok;
}

/* ── The provider paths ──────────────────────────────────────────── */

/*
 * One row per harness, and the path each of them really reads.
 *
 * This table is the *assertion*, not the implementation: the code asks
 * ai-glib and this asks whether the answer is what was verified against
 * the installed binaries and the vendor documentation.  A drift in
 * either direction shows up here.
 */
static const struct {
    const gchar *provider;
    const gchar *skill_dir;
    const gchar *command_dir;   /* NULL when the harness has no commands */
    gboolean     manifest;
} EXPECTED[] = {
    { "claude-code", ".claude/skills",   ".claude/commands",  FALSE },
    { "claude-tmux", ".claude/skills",   ".claude/commands",  FALSE },
    { "opencode",    ".opencode/skill",  ".opencode/command", FALSE },
    { "grok-build",  ".grok/skills",     NULL,                FALSE },
    { "cursor",      ".cursor/skills",   ".cursor/commands",  FALSE },
    { "antigravity", ".agents/skills",   NULL,                TRUE }
};

static void
test_each_provider_gets_its_own_paths(void)
{
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(EXPECTED); i++) {
        Fixture fixture = { 0 };
        g_auto(GStrv) skills = NULL;
        g_auto(GStrv) commands = NULL;
        g_autofree gchar *workspace = NULL;
        g_autofree gchar *want = NULL;

        fixture_setup(&fixture, EXPECTED[i].provider);
        workspace = clawt_agent_config_get_workspace(builder(&fixture));

        skills = clawt_skill_provision_paths(builder(&fixture), FALSE);
        g_assert_nonnull(skills);

        want = g_build_filename(workspace, EXPECTED[i].skill_dir, NULL);

        if (g_strcmp0(skills[0], want) != 0)
            g_error("%s: skills at %s, expected %s", EXPECTED[i].provider,
                    skills[0], want);

        commands = clawt_skill_provision_paths(builder(&fixture), TRUE);

        if (EXPECTED[i].command_dir == NULL) {
            /*
             * grok's whole slash mechanism *is* skills, and antigravity
             * has no commands concept at all.  A plausible-looking path
             * here would produce a file nothing ever reads.
             */
            if (commands != NULL && commands[0] != NULL)
                g_error("%s should have no commands directory, got %s",
                        EXPECTED[i].provider, commands[0]);
        } else {
            g_autofree gchar *want_cmd =
                g_build_filename(workspace, EXPECTED[i].command_dir, NULL);

            g_assert_nonnull(commands);

            if (g_strcmp0(commands[0], want_cmd) != 0)
                g_error("%s: commands at %s, expected %s",
                        EXPECTED[i].provider, commands[0], want_cmd);
        }

        fixture_teardown(&fixture);
    }
}

/*
 * An unknown provider is not an error to libreclaw -- it rewrites it to
 * claude-code and runs Claude Code.  So the skills have to go where
 * Claude Code looks, or the agent runs a CLI that finds nothing.
 */
static void
test_an_unknown_provider_follows_libreclaw(void)
{
    Fixture fixture = { 0 };
    g_auto(GStrv) skills = NULL;
    g_autofree gchar *workspace = NULL;
    g_autofree gchar *want = NULL;
    GLogLevelFlags fatal;

    /*
     * libreclaw warns about the unknown name and carries on, which is
     * the behaviour being pinned -- so the warning is swallowed rather
     * than avoided.  Restored immediately: g_log_set_always_fatal(0)
     * left in place would make every later test in this binary ignore a
     * real warning.
     */
    fatal = g_log_set_always_fatal(G_LOG_LEVEL_ERROR);
    g_assert_cmpstr(clawt_skill_provider_origin("something-else"), ==,
                    "claude");
    g_log_set_always_fatal(fatal);

    /* Every alias libreclaw accepts reaches the right directories. */
    g_assert_cmpstr(clawt_skill_provider_origin("claude"), ==, "claude");
    g_assert_cmpstr(clawt_skill_provider_origin("grok"), ==, "grok");
    g_assert_cmpstr(clawt_skill_provider_origin("agy"), ==, "antigravity");
    g_assert_cmpstr(clawt_skill_provider_origin("cursor-agent"), ==,
                    "cursor");
    g_assert_cmpstr(clawt_skill_provider_origin("claude-code-tmux"), ==,
                    "claude");

    fixture_setup(&fixture, "claude-code");
    workspace = clawt_agent_config_get_workspace(builder(&fixture));
    skills = clawt_skill_provision_paths(builder(&fixture), FALSE);
    want = g_build_filename(workspace, ".claude/skills", NULL);

    g_assert_cmpstr(skills[0], ==, want);

    fixture_teardown(&fixture);
}

/* ── Linking ─────────────────────────────────────────────────────── */

static void
test_a_link_is_written_at_the_providers_path(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *workspace = NULL;
    g_autofree gchar *link = NULL;
    g_autofree gchar *target = NULL;
    g_autofree gchar *marker = NULL;
    g_autoptr(GPtrArray) warnings = NULL;

    fixture_setup(&fixture, "claude-code");
    write_skill(&fixture, "release");
    g_assert_true(provision(&fixture, &warnings));

    workspace = clawt_agent_config_get_workspace(builder(&fixture));
    link = g_build_filename(workspace, ".claude/skills/release", NULL);

    g_assert_true(g_file_test(link, G_FILE_TEST_IS_SYMLINK));

    target = g_file_read_link(link, NULL);
    g_assert_nonnull(target);
    g_assert_true(clawt_path_is_within(target, fixture.skills_dir));

    /* And it resolves: a dangling link is skipped in silence by readers. */
    marker = g_build_filename(link, "SKILL.md", NULL);
    g_assert_true(g_file_test(marker, G_FILE_TEST_IS_REGULAR));

    fixture_teardown(&fixture);
}

static void
test_a_command_file_is_written_for_a_harness_that_has_them(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *workspace = NULL;
    g_autofree gchar *path = NULL;
    g_autofree gchar *text = NULL;
    g_autoptr(GPtrArray) warnings = NULL;

    fixture_setup(&fixture, "claude-code");
    write_skill(&fixture, "release");
    g_assert_true(provision(&fixture, &warnings));

    workspace = clawt_agent_config_get_workspace(builder(&fixture));
    path = g_build_filename(workspace, ".claude/commands/release.md", NULL);

    g_assert_true(g_file_test(path, G_FILE_TEST_IS_REGULAR));
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
    g_assert_nonnull(strstr(text, "clawtilla-skill: release"));

    fixture_teardown(&fixture);
}

static void
test_grok_gets_no_command_file(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *workspace = NULL;
    g_autofree gchar *link = NULL;
    g_autofree gchar *commands = NULL;
    g_autoptr(GPtrArray) warnings = NULL;

    fixture_setup(&fixture, "grok-build");
    write_skill(&fixture, "release");
    g_assert_true(provision(&fixture, &warnings));

    workspace = clawt_agent_config_get_workspace(builder(&fixture));
    link = g_build_filename(workspace, ".grok/skills/release", NULL);
    commands = g_build_filename(workspace, ".grok/commands", NULL);

    g_assert_true(g_file_test(link, G_FILE_TEST_IS_SYMLINK));
    g_assert_false(g_file_test(commands, G_FILE_TEST_EXISTS));

    fixture_teardown(&fixture);
}

/*
 * Antigravity gets the indirection the vendor shipped, not a symlink
 * whose support there nobody has written down.
 */
static void
test_antigravity_gets_a_manifest_and_not_a_link(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *workspace = NULL;
    g_autofree gchar *manifest = NULL;
    g_autofree gchar *link = NULL;
    g_autoptr(JsonParser) parser = NULL;
    g_autoptr(GPtrArray) warnings = NULL;
    JsonArray *entries;
    JsonObject *entry;

    fixture_setup(&fixture, "antigravity");
    write_skill(&fixture, "release");
    g_assert_true(provision(&fixture, &warnings));

    workspace = clawt_agent_config_get_workspace(builder(&fixture));
    manifest = g_build_filename(workspace, ".agents/skills.json", NULL);
    link = g_build_filename(workspace, ".agents/skills/release", NULL);

    g_assert_true(g_file_test(manifest, G_FILE_TEST_IS_REGULAR));
    g_assert_false(g_file_test(link, G_FILE_TEST_EXISTS));

    parser = json_parser_new();
    g_assert_true(json_parser_load_from_file(parser, manifest, NULL));

    entries = json_object_get_array_member(
        json_node_get_object(json_parser_get_root(parser)), "entries");

    g_assert_cmpuint(json_array_get_length(entries), ==, 1);

    entry = json_array_get_object_element(entries, 0);
    g_assert_cmpstr(json_object_get_string_member(entry, "name"), ==,
                    "release");
    g_assert_true(clawt_path_is_within(
        json_object_get_string_member(entry, "path"), fixture.skills_dir));

    fixture_teardown(&fixture);
}

/*
 * An entry a person put in the manifest by hand is carried across.  It
 * is the same discipline `.mcp.json` has: ours by where it points,
 * everything else untouched.
 */
static void
test_a_foreign_manifest_entry_survives(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *workspace = NULL;
    g_autofree gchar *agents_dir = NULL;
    g_autofree gchar *manifest = NULL;
    g_autoptr(JsonParser) parser = NULL;
    g_autoptr(GPtrArray) warnings = NULL;
    JsonArray *entries;
    gboolean found = FALSE;
    guint i;

    fixture_setup(&fixture, "antigravity");
    write_skill(&fixture, "release");

    workspace = clawt_agent_config_get_workspace(builder(&fixture));
    agents_dir = g_build_filename(workspace, ".agents", NULL);
    g_assert_true(clawt_ensure_dir(agents_dir, 0700, NULL));

    manifest = g_build_filename(agents_dir, "skills.json", NULL);
    g_assert_true(g_file_set_contents(
        manifest,
        "{\"entries\": [{\"name\": \"mine\", \"path\": \"/opt/mine\"}]}",
        -1, NULL));

    g_assert_true(provision(&fixture, &warnings));

    parser = json_parser_new();
    g_assert_true(json_parser_load_from_file(parser, manifest, NULL));

    entries = json_object_get_array_member(
        json_node_get_object(json_parser_get_root(parser)), "entries");

    g_assert_cmpuint(json_array_get_length(entries), ==, 2);

    for (i = 0; i < json_array_get_length(entries); i++) {
        JsonObject *entry = json_array_get_object_element(entries, i);

        if (g_strcmp0(json_object_get_string_member(entry, "path"),
                      "/opt/mine") == 0)
            found = TRUE;
    }

    g_assert_true(found);

    fixture_teardown(&fixture);
}

/* ── Ownership ───────────────────────────────────────────────────── */

/*
 * Provisioning twice leaves the workspace byte-identical.
 *
 * Not a nicety: the daemon re-renders every agent on a reload, and a
 * harness watching these directories would otherwise see a change on
 * every one of them.
 */
static void
test_provisioning_twice_changes_nothing(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *workspace = NULL;
    g_autofree gchar *command = NULL;
    g_autofree gchar *first = NULL;
    g_autofree gchar *second = NULL;
    g_autoptr(GPtrArray) warnings_a = NULL;
    g_autoptr(GPtrArray) warnings_b = NULL;
    GStatBuf before;
    GStatBuf after;
    g_autofree gchar *link = NULL;

    fixture_setup(&fixture, "claude-code");
    write_skill(&fixture, "release");
    g_assert_true(provision(&fixture, &warnings_a));

    workspace = clawt_agent_config_get_workspace(builder(&fixture));
    command = g_build_filename(workspace, ".claude/commands/release.md",
                               NULL);
    link = g_build_filename(workspace, ".claude/skills/release", NULL);

    g_assert_true(g_file_get_contents(command, &first, NULL, NULL));
    g_assert_cmpint(g_lstat(link, &before), ==, 0);

    g_assert_true(provision(&fixture, &warnings_b));

    g_assert_true(g_file_get_contents(command, &second, NULL, NULL));
    g_assert_cmpstr(first, ==, second);

    /*
     * The link was not recreated either -- asserted on the inode, since
     * an unlink-and-symlink produces a byte-identical link with a
     * different one.
     */
    g_assert_cmpint(g_lstat(link, &after), ==, 0);
    g_assert_cmpint(before.st_ino, ==, after.st_ino);

    fixture_teardown(&fixture);
}

static void
test_unassigning_removes_only_our_link(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *workspace = NULL;
    g_autofree gchar *ours = NULL;
    g_autofree gchar *theirs = NULL;
    g_autofree gchar *elsewhere = NULL;
    g_autoptr(GPtrArray) warnings = NULL;
    g_autoptr(GPtrArray) again = NULL;

    fixture_setup(&fixture, "claude-code");
    write_skill(&fixture, "release");
    g_assert_true(provision(&fixture, &warnings));

    workspace = clawt_agent_config_get_workspace(builder(&fixture));
    ours = g_build_filename(workspace, ".claude/skills/release", NULL);
    theirs = g_build_filename(workspace, ".claude/skills/handwritten", NULL);

    /*
     * A link somebody made themselves, pointing outside the library.
     * Not ours by the only test that matters -- where it points.
     */
    elsewhere = g_build_filename(fixture.dir, "elsewhere", NULL);
    g_assert_true(clawt_ensure_dir(elsewhere, 0700, NULL));
    g_assert_cmpint(symlink(elsewhere, theirs), ==, 0);

    /* Take the assignment away. */
    g_assert_true(clawt_config_set_string_list(fixture.config,
                                               "defaults.skills", NULL));
    g_assert_true(provision(&fixture, &again));

    g_assert_false(g_file_test(ours, G_FILE_TEST_IS_SYMLINK));
    g_assert_true(g_file_test(theirs, G_FILE_TEST_IS_SYMLINK));

    fixture_teardown(&fixture);
}

/*
 * And a link whose target is *spelled* through the library and then
 * walks back out of it is not ours either.
 *
 * link_is_ours() asked clawt_path_is_within(), which is a string test:
 * "<skills_dir>/../elsewhere" has the library as a prefix and a
 * separator after it, so it answered "inside" and prune_links() unlinked
 * a symlink nobody here made.  The canonical path is what says where a
 * path actually is, and it is what clawt_remove_tree() has always
 * compared against its own root.
 */
static void
test_a_link_that_escapes_the_library_is_not_ours(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *workspace = NULL;
    g_autofree gchar *theirs = NULL;
    g_autofree gchar *outside = NULL;
    g_autofree gchar *escaping = NULL;
    g_autofree gchar *read_back = NULL;
    g_autoptr(GPtrArray) warnings = NULL;
    g_autoptr(GPtrArray) again = NULL;

    fixture_setup(&fixture, "claude-code");
    write_skill(&fixture, "release");
    g_assert_true(provision(&fixture, &warnings));

    workspace = clawt_agent_config_get_workspace(builder(&fixture));
    theirs = g_build_filename(workspace, ".claude/skills/handwritten", NULL);

    outside = g_build_filename(fixture.dir, "elsewhere", NULL);
    g_assert_true(clawt_ensure_dir(outside, 0700, NULL));

    /*
     * g_build_filename() joins, it does not resolve, so the ".." stays
     * in the string -- which is the point: the spelling is inside the
     * library and the place is not.
     */
    escaping = g_build_filename(fixture.skills_dir, "..", "elsewhere", NULL);
    g_assert_cmpint(symlink(escaping, theirs), ==, 0);

    /*
     * "handwritten" is on no assignment list, so anything prune_links()
     * believes is ours goes.
     */
    g_assert_true(provision(&fixture, &again));

    g_assert_true(g_file_test(theirs, G_FILE_TEST_IS_SYMLINK));

    /* And it was left as written, not repointed at the library. */
    read_back = g_file_read_link(theirs, NULL);
    g_assert_cmpstr(read_back, ==, escaping);

    fixture_teardown(&fixture);
}

/*
 * A real directory at one of these paths is somebody's own work.  It is
 * left exactly as it is, and reported -- deleting it because it was not
 * on a list would be destroying something nobody asked us to manage.
 */
static void
test_a_real_directory_is_left_alone_and_reported(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *workspace = NULL;
    g_autofree gchar *occupied = NULL;
    g_autofree gchar *inside = NULL;
    g_autoptr(GPtrArray) warnings = NULL;
    gboolean said = FALSE;
    guint i;

    fixture_setup(&fixture, "claude-code");
    write_skill(&fixture, "release");

    workspace = clawt_agent_config_get_workspace(builder(&fixture));
    occupied = g_build_filename(workspace, ".claude/skills/release", NULL);
    g_assert_true(clawt_ensure_dir(occupied, 0700, NULL));

    inside = g_build_filename(occupied, "SKILL.md", NULL);
    g_assert_true(g_file_set_contents(inside, "mine\n", -1, NULL));

    g_assert_true(provision(&fixture, &warnings));

    g_assert_true(g_file_test(occupied, G_FILE_TEST_IS_DIR));
    g_assert_false(g_file_test(occupied, G_FILE_TEST_IS_SYMLINK));
    g_assert_true(g_file_test(inside, G_FILE_TEST_IS_REGULAR));

    for (i = 0; i < warnings->len; i++) {
        if (strstr(g_ptr_array_index(warnings, i), "not a link clawtilla")
            != NULL)
            said = TRUE;
    }

    g_assert_true(said);

    fixture_teardown(&fixture);
}

/*
 * A dangling link is repaired rather than left.
 *
 * This is exactly how the GNOME extensions bug presented: a broken
 * symlink enumerates as a symlink, every reader skips it in silence, and
 * the directory looks perfect while nothing loads.
 */
static void
test_a_dangling_link_is_repaired(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *workspace = NULL;
    g_autofree gchar *link = NULL;
    g_autofree gchar *nowhere = NULL;
    g_autofree gchar *marker = NULL;
    g_autoptr(GPtrArray) warnings = NULL;

    fixture_setup(&fixture, "claude-code");
    write_skill(&fixture, "release");

    workspace = clawt_agent_config_get_workspace(builder(&fixture));
    link = g_build_filename(workspace, ".claude/skills/release", NULL);
    nowhere = g_build_filename(fixture.skills_dir, "gone", NULL);

    {
        g_autofree gchar *parent = g_path_get_dirname(link);

        g_assert_true(clawt_ensure_dir(parent, 0700, NULL));
    }
    g_assert_cmpint(symlink(nowhere, link), ==, 0);
    g_assert_true(g_file_test(link, G_FILE_TEST_IS_SYMLINK));
    g_assert_false(g_file_test(link, G_FILE_TEST_EXISTS));

    g_assert_true(provision(&fixture, &warnings));

    marker = g_build_filename(link, "SKILL.md", NULL);
    g_assert_true(g_file_test(link, G_FILE_TEST_IS_SYMLINK));
    g_assert_true(g_file_test(marker, G_FILE_TEST_IS_REGULAR));

    fixture_teardown(&fixture);
}

/*
 * A dangling link for a skill that is no longer assigned is *removed*,
 * not merely left broken.  Same reasoning: the harness cannot tell a
 * broken link from an absent one, but a person reading `ls` can, and
 * would conclude the skill was still there.
 */
static void
test_a_dangling_link_for_an_unassigned_skill_is_removed(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *workspace = NULL;
    g_autofree gchar *link = NULL;
    g_autofree gchar *nowhere = NULL;
    g_autofree gchar *parent = NULL;
    g_autoptr(GPtrArray) warnings = NULL;

    fixture_setup(&fixture, "claude-code");
    g_assert_true(clawt_config_set_string_list(fixture.config,
                                               "defaults.skills", NULL));

    workspace = clawt_agent_config_get_workspace(builder(&fixture));
    parent = g_build_filename(workspace, ".claude/skills", NULL);
    g_assert_true(clawt_ensure_dir(parent, 0700, NULL));

    link = g_build_filename(parent, "gone", NULL);
    nowhere = g_build_filename(fixture.skills_dir, "gone", NULL);
    g_assert_cmpint(symlink(nowhere, link), ==, 0);

    g_assert_true(provision(&fixture, &warnings));

    g_assert_false(g_file_test(link, G_FILE_TEST_IS_SYMLINK));

    fixture_teardown(&fixture);
}

static void
test_a_disabled_skill_is_not_linked(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *workspace = NULL;
    g_autofree gchar *link = NULL;
    g_autoptr(GPtrArray) warnings = NULL;
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture, "claude-code");
    write_skill(&fixture, "release");
    g_assert_true(clawt_skill_library_set_enabled(fixture.library, "release",
                                                  FALSE, &error));
    g_assert_no_error(error);

    g_assert_true(provision(&fixture, &warnings));

    workspace = clawt_agent_config_get_workspace(builder(&fixture));
    link = g_build_filename(workspace, ".claude/skills/release", NULL);

    /*
     * The point of the whole posture: an imported skill is assigned and
     * inert until somebody enables it, so nothing about it may reach a
     * directory a harness scans.
     */
    g_assert_false(g_file_test(link, G_FILE_TEST_EXISTS));

    fixture_teardown(&fixture);
}

static void
test_a_removed_skill_takes_its_link_with_it(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *workspace = NULL;
    g_autofree gchar *link = NULL;
    g_autofree gchar *command = NULL;
    g_autoptr(GPtrArray) warnings = NULL;
    g_autoptr(GPtrArray) again = NULL;
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture, "claude-code");
    write_skill(&fixture, "release");
    g_assert_true(provision(&fixture, &warnings));

    workspace = clawt_agent_config_get_workspace(builder(&fixture));
    link = g_build_filename(workspace, ".claude/skills/release", NULL);
    command = g_build_filename(workspace, ".claude/commands/release.md",
                               NULL);

    g_assert_true(g_file_test(link, G_FILE_TEST_IS_SYMLINK));

    g_assert_true(clawt_skill_library_remove(fixture.library, "release",
                                             &error));
    g_assert_no_error(error);
    g_assert_true(provision(&fixture, &again));

    g_assert_false(g_file_test(link, G_FILE_TEST_IS_SYMLINK));
    g_assert_false(g_file_test(command, G_FILE_TEST_EXISTS));

    fixture_teardown(&fixture);
}

static void
test_a_foreign_command_file_is_left_alone(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *workspace = NULL;
    g_autofree gchar *dir = NULL;
    g_autofree gchar *path = NULL;
    g_autofree gchar *text = NULL;
    g_autoptr(GPtrArray) warnings = NULL;

    fixture_setup(&fixture, "claude-code");
    write_skill(&fixture, "release");

    workspace = clawt_agent_config_get_workspace(builder(&fixture));
    dir = g_build_filename(workspace, ".claude/commands", NULL);
    g_assert_true(clawt_ensure_dir(dir, 0700, NULL));

    path = g_build_filename(dir, "release.md", NULL);
    g_assert_true(g_file_set_contents(path, "mine, hands off\n", -1, NULL));

    g_assert_true(provision(&fixture, &warnings));

    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
    g_assert_cmpstr(text, ==, "mine, hands off\n");

    fixture_teardown(&fixture);
}

/* ── What the agent is told ──────────────────────────────────────── */

static void
test_the_description_names_the_skills(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GPtrArray) bindings = NULL;
    g_autofree gchar *text = NULL;

    fixture_setup(&fixture, "claude-code");
    write_skill(&fixture, "release");

    bindings = clawt_skill_resolve_for_agent(fixture.config,
                                             builder(&fixture),
                                             fixture.library);
    text = clawt_skill_provision_describe(bindings);

    g_assert_nonnull(strstr(text, "release"));
    g_assert_nonnull(strstr(text, "Cut a release"));

    /*
     * And it says the description is a summary, because a model that
     * acts on the summary does a plausible-looking version of the
     * procedure rather than the procedure.
     */
    g_assert_nonnull(strstr(text, "summary"));

    fixture_teardown(&fixture);
}

static void
test_having_no_skills_is_said_out_loud(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GPtrArray) bindings = NULL;
    g_autofree gchar *text = NULL;

    fixture_setup(&fixture, "claude-code");
    g_assert_true(clawt_config_set_string_list(fixture.config,
                                               "defaults.skills", NULL));
    clawt_skill_library_scan(fixture.library);

    bindings = clawt_skill_resolve_for_agent(fixture.config,
                                             builder(&fixture),
                                             fixture.library);
    text = clawt_skill_provision_describe(bindings);

    /*
     * An empty section reads as "clawtilla has not worked this out
     * yet", and an agent that suspects it has procedures it cannot see
     * goes looking for them instead of doing the work.
     */
    g_assert_nonnull(strstr(text, "You have none"));

    fixture_teardown(&fixture);
}

/*
 * The markers go *in* the section.  replace_region() swaps everything
 * from the begin marker to the end marker inclusive, so a section that
 * omitted them would remove them -- and the next start, finding no
 * region, would append a second copy, growing the file per daemon start.
 */
static void
test_the_tools_org_region_does_not_duplicate(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GPtrArray) bindings = NULL;
    g_autofree gchar *text = NULL;
    g_autofree gchar *path = NULL;
    g_autofree gchar *contents = NULL;
    g_autoptr(GError) error = NULL;
    const gchar *first;
    const gchar *second;

    fixture_setup(&fixture, "claude-code");
    write_skill(&fixture, "release");

    g_assert_true(clawt_workspace_scaffold(builder(&fixture), &error));
    g_assert_no_error(error);

    bindings = clawt_skill_resolve_for_agent(fixture.config,
                                             builder(&fixture),
                                             fixture.library);
    text = clawt_skill_provision_describe(bindings);

    g_assert_true(clawt_workspace_update_skills(builder(&fixture), text,
                                                &error));
    g_assert_no_error(error);
    g_assert_true(clawt_workspace_update_skills(builder(&fixture), text,
                                                &error));
    g_assert_no_error(error);
    g_assert_true(clawt_workspace_update_skills(builder(&fixture), text,
                                                &error));
    g_assert_no_error(error);

    path = clawt_workspace_file_path(builder(&fixture), "TOOLS.org");
    g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));

    first = strstr(contents, "BEGIN clawtilla skills");
    g_assert_nonnull(first);

    second = strstr(first + 1, "BEGIN clawtilla skills");
    g_assert_null(second);

    g_assert_nonnull(strstr(contents, "END clawtilla skills"));

    fixture_teardown(&fixture);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/skill-provision/paths",
                    test_each_provider_gets_its_own_paths);
    g_test_add_func("/skill-provision/unknown-provider",
                    test_an_unknown_provider_follows_libreclaw);
    g_test_add_func("/skill-provision/link",
                    test_a_link_is_written_at_the_providers_path);
    g_test_add_func("/skill-provision/command-file",
                    test_a_command_file_is_written_for_a_harness_that_has_them);
    g_test_add_func("/skill-provision/grok-no-commands",
                    test_grok_gets_no_command_file);
    g_test_add_func("/skill-provision/antigravity-manifest",
                    test_antigravity_gets_a_manifest_and_not_a_link);
    g_test_add_func("/skill-provision/manifest-carry",
                    test_a_foreign_manifest_entry_survives);

    g_test_add_func("/skill-provision/idempotent",
                    test_provisioning_twice_changes_nothing);
    g_test_add_func("/skill-provision/revoke",
                    test_unassigning_removes_only_our_link);
    g_test_add_func("/skill-provision/escaping-link",
                    test_a_link_that_escapes_the_library_is_not_ours);
    g_test_add_func("/skill-provision/real-directory",
                    test_a_real_directory_is_left_alone_and_reported);
    g_test_add_func("/skill-provision/dangling-repaired",
                    test_a_dangling_link_is_repaired);
    g_test_add_func("/skill-provision/dangling-removed",
                    test_a_dangling_link_for_an_unassigned_skill_is_removed);
    g_test_add_func("/skill-provision/disabled-not-linked",
                    test_a_disabled_skill_is_not_linked);
    g_test_add_func("/skill-provision/removed-skill",
                    test_a_removed_skill_takes_its_link_with_it);
    g_test_add_func("/skill-provision/foreign-command",
                    test_a_foreign_command_file_is_left_alone);

    g_test_add_func("/skill-provision/describe",
                    test_the_description_names_the_skills);
    g_test_add_func("/skill-provision/describe-empty",
                    test_having_no_skills_is_said_out_loud);
    g_test_add_func("/skill-provision/tools-org-once",
                    test_the_tools_org_region_does_not_duplicate);

    return g_test_run();
}
