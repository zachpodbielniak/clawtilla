/*
 * test-skill.c - The library on disk, and who gets what
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Two things are proved here that no amount of reading the code will
 * show.  The first is that an imported skill lands disabled and stays
 * that way -- the whole security posture rests on it, and it is one
 * assignment away from being wrong.  The second is that assignment
 * resolves through one function across all three places it can be
 * written, because a second resolver is the failure this project has
 * already paid for with integrations.
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
fixture_setup(Fixture *fixture, const gchar *body)
{
    g_autofree gchar *yaml = NULL;
    g_autoptr(GError) error = NULL;

    fixture->dir = g_dir_make_tmp("clawt-skill-XXXXXX", NULL);
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
        "skills:\n"
        "  dir: \"%s\"\n"
        "%s",
        fixture->dir, fixture->dir, fixture->dir, fixture->dir,
        fixture->skills_dir, body != NULL ? body : "");

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

/* Write a skill directory by hand, the way a person would. */
static gchar *
write_skill(const gchar *root, const gchar *name, const gchar *description,
            const gchar *body)
{
    gchar *dir = g_build_filename(root, name, NULL);
    g_autofree gchar *file = g_build_filename(dir, "SKILL.md", NULL);
    g_autofree gchar *text = NULL;

    g_assert_true(clawt_ensure_dir(dir, 0700, NULL));

    text = g_strdup_printf("---\nname: %s\ndescription: %s\n---\n\n%s\n",
                           name, description,
                           body != NULL ? body : "Do the thing.");

    g_assert_true(g_file_set_contents(file, text, -1, NULL));

    return dir;
}

/* ── Scanning ────────────────────────────────────────────────────── */

static void
test_a_skill_on_disk_is_found(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GPtrArray) skills = NULL;
    g_autofree gchar *dir = NULL;

    fixture_setup(&fixture, NULL);
    dir = write_skill(fixture.skills_dir, "release", "Cut a release", NULL);

    clawt_skill_library_scan(fixture.library);
    skills = clawt_skill_library_list(fixture.library);

    g_assert_cmpuint(skills->len, ==, 1);
    g_assert_cmpstr(clawt_skill_get_name(g_ptr_array_index(skills, 0)), ==,
                    "release");

    /*
     * A skill somebody wrote here by hand is enabled: putting it in the
     * directory *is* the review.  Only import and synthesis land
     * disabled.
     */
    g_assert_true(
        clawt_skill_get_enabled(g_ptr_array_index(skills, 0)));

    fixture_teardown(&fixture);
}

static void
test_a_missing_directory_is_an_empty_library_not_an_error(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-skill-XXXXXX", NULL);
    g_autofree gchar *absent = g_build_filename(dir, "nope", NULL);
    g_autoptr(ClawtSkillLibrary) library = clawt_skill_library_new(absent);
    g_autoptr(GPtrArray) skills = NULL;

    clawt_skill_library_scan(library);
    skills = clawt_skill_library_list(library);

    g_assert_cmpuint(skills->len, ==, 0);
    g_assert_cmpuint(clawt_skill_library_get_problems(library)->len, ==, 0);

    /*
     * And it is not created.  A read with a side effect is how a typo
     * in the path leaves an empty directory for somebody to puzzle over
     * a week later.
     */
    g_assert_false(g_file_test(absent, G_FILE_TEST_EXISTS));

    clawt_test_remove_tree(dir);
}

static void
test_a_directory_with_a_bad_name_is_reported(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *dir = NULL;
    g_autofree gchar *file = NULL;

    fixture_setup(&fixture, NULL);

    dir = g_build_filename(fixture.skills_dir, "Legal-Review", NULL);
    g_assert_true(clawt_ensure_dir(dir, 0700, NULL));
    file = g_build_filename(dir, "SKILL.md", NULL);
    g_assert_true(g_file_set_contents(
        file, "---\nname: legal-review\ndescription: d\n---\n", -1, NULL));

    clawt_skill_library_scan(fixture.library);

    /*
     * Reported rather than silently absent.  A skill missing from every
     * listing looks like a skill that was never written.
     */
    g_assert_cmpuint(clawt_skill_library_get_problems(fixture.library)->len,
                     ==, 1);

    fixture_teardown(&fixture);
}

static void
test_a_malformed_skill_is_reported_not_dropped(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *dir = NULL;
    g_autofree gchar *file = NULL;
    g_autoptr(GPtrArray) skills = NULL;
    GPtrArray *problems;

    fixture_setup(&fixture, NULL);

    dir = g_build_filename(fixture.skills_dir, "broken", NULL);
    g_assert_true(clawt_ensure_dir(dir, 0700, NULL));
    file = g_build_filename(dir, "SKILL.md", NULL);
    g_assert_true(g_file_set_contents(file, "no front matter here", -1,
                                      NULL));

    clawt_skill_library_scan(fixture.library);
    skills = clawt_skill_library_list(fixture.library);
    problems = clawt_skill_library_get_problems(fixture.library);

    g_assert_cmpuint(skills->len, ==, 0);
    g_assert_cmpuint(problems->len, ==, 1);
    g_assert_nonnull(strstr(g_ptr_array_index(problems, 0), "broken"));

    fixture_teardown(&fixture);
}

/* ── Creating and importing ──────────────────────────────────────── */

static void
test_a_created_skill_is_enabled(void)
{
    Fixture fixture = { 0 };
    ClawtSkill *skill;
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture, NULL);
    clawt_skill_library_scan(fixture.library);

    skill = clawt_skill_library_create(fixture.library, "release",
                                       "Cut a release", NULL, &error);

    g_assert_no_error(error);
    g_assert_true(clawt_skill_get_enabled(skill));
    g_assert_cmpint(clawt_skill_get_source(skill), ==,
                    CLAWT_SKILL_SOURCE_USER);

    fixture_teardown(&fixture);
}

static void
test_a_created_skill_needs_a_description(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autofree gchar *directory = NULL;

    fixture_setup(&fixture, NULL);
    clawt_skill_library_scan(fixture.library);

    g_assert_null(clawt_skill_library_create(fixture.library, "release", NULL,
                                             NULL, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);

    /* And nothing half-made was left behind for the next scan to find. */
    directory = g_build_filename(fixture.skills_dir, "release", NULL);
    g_assert_false(g_file_test(directory, G_FILE_TEST_EXISTS));

    fixture_teardown(&fixture);
}

/*
 * The assertion the whole security posture rests on.
 *
 * A registry audit found working exfiltration payloads in between 2% and
 * 13% of public skills.  Nothing an import brings in may reach a prompt
 * before a person has read it, and that is one assignment away from
 * being wrong in either direction.
 */
static void
test_an_imported_skill_lands_disabled(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *source = NULL;
    ClawtSkill *skill;
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture, NULL);
    clawt_skill_library_scan(fixture.library);

    source = write_skill(fixture.dir, "release", "Cut a release", NULL);

    skill = clawt_skill_library_import(fixture.library, source,
                                       "https://example.invalid/release",
                                       &error);

    g_assert_no_error(error);
    g_assert_false(clawt_skill_get_enabled(skill));
    g_assert_cmpint(clawt_skill_get_source(skill), ==,
                    CLAWT_SKILL_SOURCE_IMPORTED);

    /* And it survives a rescan as disabled, not merely in memory. */
    clawt_skill_library_scan(fixture.library);
    g_assert_false(clawt_skill_get_enabled(
        clawt_skill_library_lookup(fixture.library, "release")));

    fixture_teardown(&fixture);
}

static void
test_an_import_records_its_provenance(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *source = NULL;
    ClawtSkill *skill;
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture, NULL);
    clawt_skill_library_scan(fixture.library);
    source = write_skill(fixture.dir, "release", "Cut a release", NULL);

    skill = clawt_skill_library_import(fixture.library, source,
                                       "https://example.invalid/release",
                                       &error);

    g_assert_no_error(error);
    g_assert_cmpstr(clawt_skill_get_origin_url(skill), ==,
                    "https://example.invalid/release");
    g_assert_cmpuint(strlen(clawt_skill_get_digest(skill)), ==, 64);
    g_assert_cmpint(clawt_skill_get_imported_at(skill), >, 0);

    fixture_teardown(&fixture);
}

/*
 * Markdown only, and the skipped list is *shown*.
 *
 * Skipping silently is worse than not skipping: a skill whose steps say
 * "run scripts/setup.sh" fails in a way that reads as clawtilla being
 * broken rather than as a deliberate refusal.
 */
static void
test_an_import_skips_scripts_and_says_which(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *source = NULL;
    g_autofree gchar *script = NULL;
    g_autofree gchar *reference = NULL;
    g_autofree gchar *landed = NULL;
    ClawtSkill *skill;
    GPtrArray *skipped;
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture, NULL);
    clawt_skill_library_scan(fixture.library);

    source = write_skill(fixture.dir, "release", "Cut a release", NULL);

    script = g_build_filename(source, "setup.sh", NULL);
    g_assert_true(g_file_set_contents(script, "#!/bin/sh\necho hi\n", -1,
                                      NULL));

    reference = g_build_filename(source, "REFERENCE.md", NULL);
    g_assert_true(g_file_set_contents(reference, "# notes\n", -1, NULL));

    skill = clawt_skill_library_import(fixture.library, source, NULL, &error);
    g_assert_no_error(error);

    skipped = clawt_skill_get_skipped(skill);
    g_assert_cmpuint(skipped->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(skipped, 0), ==, "setup.sh");

    landed = g_build_filename(clawt_skill_get_directory(skill), "setup.sh",
                              NULL);
    g_assert_false(g_file_test(landed, G_FILE_TEST_EXISTS));

    /* Sibling markdown does come across -- it is text and runs nothing. */
    g_free(landed);
    landed = g_build_filename(clawt_skill_get_directory(skill),
                              "REFERENCE.md", NULL);
    g_assert_true(g_file_test(landed, G_FILE_TEST_IS_REGULAR));

    /* And the skipped list survives a rescan, so a listing can show it. */
    clawt_skill_library_scan(fixture.library);
    g_assert_cmpuint(
        clawt_skill_get_skipped(
            clawt_skill_library_lookup(fixture.library, "release"))->len,
        ==, 1);

    fixture_teardown(&fixture);
}

static void
test_an_edited_skill_is_noticed(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *source = NULL;
    g_autofree gchar *file = NULL;
    ClawtSkill *skill;
    g_autoptr(GError) error = NULL;
    gboolean warned = FALSE;
    GPtrArray *warnings;
    guint i;

    fixture_setup(&fixture, NULL);
    clawt_skill_library_scan(fixture.library);
    source = write_skill(fixture.dir, "release", "Cut a release", NULL);

    skill = clawt_skill_library_import(fixture.library, source, NULL, &error);
    g_assert_no_error(error);

    file = g_build_filename(clawt_skill_get_directory(skill), "SKILL.md",
                            NULL);
    g_assert_true(g_file_set_contents(
        file,
        "---\nname: release\ndescription: Cut a release\n---\n\n"
        "Do something else entirely.\n", -1, NULL));

    clawt_skill_library_scan(fixture.library);

    warnings = clawt_skill_get_warnings(
        clawt_skill_library_lookup(fixture.library, "release"));

    for (i = 0; i < warnings->len; i++) {
        if (strstr(g_ptr_array_index(warnings, i), "changed since") != NULL)
            warned = TRUE;
    }

    g_assert_true(warned);

    fixture_teardown(&fixture);
}

static void
test_enable_and_remove_round_trip(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *dir = NULL;
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture, NULL);
    dir = write_skill(fixture.skills_dir, "release", "Cut a release", NULL);
    clawt_skill_library_scan(fixture.library);

    g_assert_true(clawt_skill_library_set_enabled(fixture.library, "release",
                                                  FALSE, &error));
    g_assert_no_error(error);

    clawt_skill_library_scan(fixture.library);
    g_assert_false(clawt_skill_get_enabled(
        clawt_skill_library_lookup(fixture.library, "release")));

    g_assert_true(clawt_skill_library_remove(fixture.library, "release",
                                             &error));
    g_assert_no_error(error);
    g_assert_false(g_file_test(dir, G_FILE_TEST_EXISTS));
    g_assert_null(clawt_skill_library_lookup(fixture.library, "release"));

    fixture_teardown(&fixture);
}

/*
 * The name gate again, on every entry point rather than only at the
 * edge.  A rule enforced at one call site is a rule about that call
 * site.
 */
static void
test_the_library_refuses_a_traversal_name(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) create_error = NULL;
    g_autoptr(GError) remove_error = NULL;

    fixture_setup(&fixture, NULL);
    clawt_skill_library_scan(fixture.library);

    g_assert_null(clawt_skill_library_create(fixture.library, "../escape",
                                             "d", NULL, &create_error));
    g_assert_error(create_error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);

    g_assert_false(clawt_skill_library_remove(fixture.library, "..",
                                              &remove_error));
    g_assert_error(remove_error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);

    fixture_teardown(&fixture);
}

/* ── Resolution ──────────────────────────────────────────────────── */

static void
setup_three_levels(Fixture *fixture)
{
    g_autofree gchar *yaml = NULL;
    g_autoptr(GError) error = NULL;

    fixture->dir = g_dir_make_tmp("clawt-skill-XXXXXX", NULL);
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
        "  skills: [fleet-skill]\n"
        "skills:\n"
        "  dir: \"%s\"\n"
        "teams:\n"
        "  - id: engineering\n"
        "    skills: [team-skill]\n"
        "agents:\n"
        "  - id: builder\n"
        "    team: engineering\n"
        "    skills: [agent-skill]\n"
        "  - id: idler\n",
        fixture->dir, fixture->dir, fixture->dir, fixture->dir,
        fixture->skills_dir);

    fixture->config = clawt_config_load_from_string(yaml, &error);
    g_assert_no_error(error);

    fixture->library = clawt_skill_library_new(fixture->skills_dir);
}

static ClawtSkillBinding *
binding_named(GPtrArray *bindings, const gchar *name)
{
    guint i;

    for (i = 0; i < bindings->len; i++) {
        ClawtSkillBinding *binding = g_ptr_array_index(bindings, i);

        if (g_strcmp0(clawt_skill_binding_get_name(binding), name) == 0)
            return binding;
    }

    return NULL;
}

static void
test_all_three_levels_reach_the_agent(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GPtrArray) bindings = NULL;
    g_autofree gchar *a = NULL;
    g_autofree gchar *t = NULL;
    g_autofree gchar *f = NULL;

    setup_three_levels(&fixture);
    a = write_skill(fixture.skills_dir, "agent-skill", "a", NULL);
    t = write_skill(fixture.skills_dir, "team-skill", "t", NULL);
    f = write_skill(fixture.skills_dir, "fleet-skill", "f", NULL);
    clawt_skill_library_scan(fixture.library);

    bindings = clawt_skill_resolve_for_agent(
        fixture.config, clawt_config_get_agent(fixture.config, "builder"),
        fixture.library);

    g_assert_cmpuint(bindings->len, ==, 3);
    g_assert_cmpstr(
        clawt_skill_binding_get_origin(binding_named(bindings,
                                                     "agent-skill")),
        ==, "agent");
    g_assert_cmpstr(
        clawt_skill_binding_get_origin(binding_named(bindings, "team-skill")),
        ==, "team");
    g_assert_cmpstr(
        clawt_skill_binding_get_origin(binding_named(bindings,
                                                     "fleet-skill")),
        ==, "fleet");

    fixture_teardown(&fixture);
}

static void
test_an_agent_with_no_team_still_gets_the_fleet_list(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GPtrArray) bindings = NULL;
    g_autofree gchar *f = NULL;

    setup_three_levels(&fixture);
    f = write_skill(fixture.skills_dir, "fleet-skill", "f", NULL);
    clawt_skill_library_scan(fixture.library);

    bindings = clawt_skill_resolve_for_agent(
        fixture.config, clawt_config_get_agent(fixture.config, "idler"),
        fixture.library);

    g_assert_cmpuint(bindings->len, ==, 1);
    g_assert_cmpstr(clawt_skill_binding_get_name(
                        g_ptr_array_index(bindings, 0)), ==, "fleet-skill");

    fixture_teardown(&fixture);
}

/*
 * A skill reached two ways is one skill, counted once, attributed to the
 * most specific place it was asked for.
 *
 * Counting it twice would double every link, every line in TOOLS.org and
 * every entry in a manifest -- which nothing would report, because each
 * of them is individually correct.
 */
static void
test_a_skill_reached_two_ways_is_counted_once(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *yaml = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(GPtrArray) bindings = NULL;
    g_autofree gchar *s = NULL;

    fixture.dir = g_dir_make_tmp("clawt-skill-XXXXXX", NULL);
    fixture.skills_dir = g_build_filename(fixture.dir, "skills", NULL);
    g_assert_true(clawt_ensure_dir(fixture.skills_dir, 0700, NULL));

    yaml = g_strdup_printf(
        "daemon:\n"
        "  state_dir: \"%s/state\"\n"
        "  socket: \"%s/sock\"\n"
        "  automation_dir: \"%s/pods\"\n"
        "  tailscale: false\n"
        "defaults:\n"
        "  workspace_root: \"%s/agents\"\n"
        "  skills: [shared]\n"
        "skills:\n"
        "  dir: \"%s\"\n"
        "teams:\n"
        "  - id: engineering\n"
        "    skills: [shared]\n"
        "agents:\n"
        "  - id: builder\n"
        "    team: engineering\n"
        "    skills: [shared]\n",
        fixture.dir, fixture.dir, fixture.dir, fixture.dir,
        fixture.skills_dir);

    fixture.config = clawt_config_load_from_string(yaml, &error);
    g_assert_no_error(error);
    fixture.library = clawt_skill_library_new(fixture.skills_dir);

    s = write_skill(fixture.skills_dir, "shared", "s", NULL);
    clawt_skill_library_scan(fixture.library);

    bindings = clawt_skill_resolve_for_agent(
        fixture.config, clawt_config_get_agent(fixture.config, "builder"),
        fixture.library);

    g_assert_cmpuint(bindings->len, ==, 1);
    g_assert_cmpstr(clawt_skill_binding_get_origin(
                        g_ptr_array_index(bindings, 0)), ==, "agent");

    fixture_teardown(&fixture);
}

/*
 * A selector matching nothing must say so.  An agent quietly missing the
 * procedure it was configured with looks exactly like one that has it
 * and ignored it.
 */
static void
test_a_selector_matching_nothing_is_reported(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GPtrArray) bindings = NULL;
    g_autoptr(GPtrArray) warnings = NULL;

    setup_three_levels(&fixture);
    clawt_skill_library_scan(fixture.library);

    bindings = clawt_skill_resolve_for_agent(
        fixture.config, clawt_config_get_agent(fixture.config, "builder"),
        fixture.library);

    g_assert_cmpuint(bindings->len, ==, 3);
    g_assert_null(clawt_skill_binding_get_skill(
        g_ptr_array_index(bindings, 0)));
    g_assert_false(clawt_skill_binding_is_active(
        g_ptr_array_index(bindings, 0)));

    warnings = clawt_skill_bindings_warnings(bindings, "builder");
    g_assert_cmpuint(warnings->len, ==, 3);
    g_assert_nonnull(strstr(g_ptr_array_index(warnings, 0),
                            "reaches nobody"));

    fixture_teardown(&fixture);
}

/*
 * "Assigned" and "in effect" are different, and a disabled skill is the
 * ordinary case for anything imported.  Reported as its own sentence,
 * because phrasing it like the missing case sends somebody looking for a
 * file that is right there.
 */
static void
test_a_disabled_skill_is_assigned_and_not_active(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GPtrArray) bindings = NULL;
    g_autoptr(GPtrArray) warnings = NULL;
    g_autofree gchar *a = NULL;
    g_autoptr(GError) error = NULL;

    setup_three_levels(&fixture);
    a = write_skill(fixture.skills_dir, "agent-skill", "a", NULL);
    clawt_skill_library_scan(fixture.library);
    g_assert_true(clawt_skill_library_set_enabled(fixture.library,
                                                  "agent-skill", FALSE,
                                                  &error));

    bindings = clawt_skill_resolve_for_agent(
        fixture.config, clawt_config_get_agent(fixture.config, "builder"),
        fixture.library);

    g_assert_nonnull(clawt_skill_binding_get_skill(
        binding_named(bindings, "agent-skill")));
    g_assert_false(clawt_skill_binding_is_active(
        binding_named(bindings, "agent-skill")));

    warnings = clawt_skill_bindings_warnings(bindings, "builder");
    g_assert_true(warnings->len >= 1);

    {
        gboolean said = FALSE;
        guint i;

        for (i = 0; i < warnings->len; i++) {
            if (strstr(g_ptr_array_index(warnings, i), "not enabled yet")
                != NULL)
                said = TRUE;
        }

        g_assert_true(said);
    }

    fixture_teardown(&fixture);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/skill/found", test_a_skill_on_disk_is_found);
    g_test_add_func("/skill/missing-directory",
                    test_a_missing_directory_is_an_empty_library_not_an_error);
    g_test_add_func("/skill/bad-directory-name",
                    test_a_directory_with_a_bad_name_is_reported);
    g_test_add_func("/skill/malformed",
                    test_a_malformed_skill_is_reported_not_dropped);

    g_test_add_func("/skill/created-enabled", test_a_created_skill_is_enabled);
    g_test_add_func("/skill/created-needs-description",
                    test_a_created_skill_needs_a_description);
    g_test_add_func("/skill/imported-disabled",
                    test_an_imported_skill_lands_disabled);
    g_test_add_func("/skill/provenance",
                    test_an_import_records_its_provenance);
    g_test_add_func("/skill/markdown-only",
                    test_an_import_skips_scripts_and_says_which);
    g_test_add_func("/skill/edited-after-import",
                    test_an_edited_skill_is_noticed);
    g_test_add_func("/skill/enable-remove",
                    test_enable_and_remove_round_trip);
    g_test_add_func("/skill/library-name-gate",
                    test_the_library_refuses_a_traversal_name);

    g_test_add_func("/skill/three-levels",
                    test_all_three_levels_reach_the_agent);
    g_test_add_func("/skill/no-team",
                    test_an_agent_with_no_team_still_gets_the_fleet_list);
    g_test_add_func("/skill/counted-once",
                    test_a_skill_reached_two_ways_is_counted_once);
    g_test_add_func("/skill/selector-misses",
                    test_a_selector_matching_nothing_is_reported);
    g_test_add_func("/skill/assigned-but-disabled",
                    test_a_disabled_skill_is_assigned_and_not_active);

    return g_test_run();
}
