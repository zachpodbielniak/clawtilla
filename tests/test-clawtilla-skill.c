/*
 * test-clawtilla-skill.c - The bundled /clawtilla skill reaches who runs the fleet
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The clawtilla skill is the manual for changing a fleet, and the agents
 * that change one -- the chief of staff, anyone with tools.manage_fleet,
 * the office's custodians through tools.office -- run on the daemon's
 * machine with no docs beside them.  So the daemon carries it and hands it
 * to exactly them.  Checked here: it installs whole (nested reference
 * directories included) and loads cleanly, it is implied for those three
 * and nobody else, and an upgrade removes a file the newer build no
 * longer ships instead of leaving a stale reference to be read.
 */

#include "clawtilla.h"
#include "clawt-test-util.h"
#include <glib/gstdio.h>
#include <string.h>

static gboolean
has_skill(ClawtConfig *config, const gchar *agent_id, const gchar *name)
{
    g_autoptr(GPtrArray) bindings = clawt_skill_resolve_for_agent(
        config, clawt_config_get_agent(config, agent_id), NULL);
    guint i;

    for (i = 0; bindings != NULL && i < bindings->len; i++)
        if (g_strcmp0(clawt_skill_binding_get_name(g_ptr_array_index(bindings, i)), name) == 0)
            return TRUE;

    return FALSE;
}

/* Installed with its generated config reference, and loads with no problem. */
static void
test_installs_and_loads(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-skills-XXXXXX", NULL);
    g_autofree gchar *index = g_build_filename(dir, "clawtilla", "references",
                                               "config-keys.md", NULL);
    g_autofree gchar *section = g_build_filename(dir, "clawtilla", "references",
                                                 "config", "orchestration.md", NULL);
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtSkillLibrary) library = NULL;
    ClawtSkill *found;
    g_autofree gchar *text = NULL;

    g_assert_true(clawt_skill_install_bundled(dir, &error));
    g_assert_no_error(error);
    g_assert_true(g_file_test(index, G_FILE_TEST_IS_REGULAR));
    g_assert_true(g_file_test(section, G_FILE_TEST_IS_REGULAR));

    /* The embedded copy is the generated one, so it names a real key. */
    g_assert_true(g_file_get_contents(index, &text, NULL, NULL));
    g_assert_nonnull(strstr(text, "`orchestration.max_hops`"));

    library = clawt_skill_library_new(dir);
    clawt_skill_library_scan(library);
    found = clawt_skill_library_lookup(library, CLAWT_SKILL_BUNDLED_CLAWTILLA);
    g_assert_nonnull(found);
    g_assert_true(clawt_skill_get_enabled(found));
    g_assert_nonnull(strstr(clawt_skill_get_description(found), "clawtilla"));
    g_assert_cmpuint(clawt_skill_library_get_problems(library)->len, ==, 0);

    clawt_test_remove_tree(dir);
}

/* The chief, a fleet manager and a custodian get it; a plain agent does not. */
static void
test_implied_for_who_runs_the_fleet(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtConfig) config = clawt_config_load_from_string(
        "agents:\n"
        "  - id: chief\n"
        "    chief_of_staff: true\n"
        "  - id: admin\n"
        "    tools:\n"
        "      manage_fleet: true\n"
        "  - id: janitor\n"
        "    tools:\n"
        "      office: true\n"
        "  - id: plain\n", &error);

    g_assert_no_error(error);
    g_assert_true(has_skill(config, "chief", CLAWT_SKILL_BUNDLED_CLAWTILLA));
    g_assert_true(has_skill(config, "admin", CLAWT_SKILL_BUNDLED_CLAWTILLA));
    g_assert_true(has_skill(config, "janitor", CLAWT_SKILL_BUNDLED_CLAWTILLA));
    g_assert_false(has_skill(config, "plain", CLAWT_SKILL_BUNDLED_CLAWTILLA));
}

/*
 * The chief's tools.office comes from its role, and turning it off for a
 * chief does not take the manual away: being the chief is reason enough.
 */
static void
test_chief_keeps_it_without_office(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtConfig) config = clawt_config_load_from_string(
        "agents:\n"
        "  - id: chief\n"
        "    chief_of_staff: true\n"
        "    tools:\n"
        "      office: false\n", &error);

    g_assert_no_error(error);
    g_assert_true(has_skill(config, "chief", CLAWT_SKILL_BUNDLED_CLAWTILLA));
}

/* An older copy of ours is replaced whole: a file we no longer ship goes. */
static void
test_upgrade_removes_stale_files(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-skills-XXXXXX", NULL);
    g_autofree gchar *skill_dir = g_build_filename(dir, "clawtilla", NULL);
    g_autofree gchar *stale = g_build_filename(skill_dir, "references", "config",
                                               "gone.md", NULL);
    g_autofree gchar *marker = g_build_filename(skill_dir, CLAWT_SKILL_BUNDLED_MARKER, NULL);
    g_autofree gchar *skill = g_build_filename(skill_dir, "SKILL.md", NULL);
    g_autoptr(GError) error = NULL;

    g_assert_true(clawt_skill_install_bundled(dir, &error));
    g_assert_true(g_file_set_contents(stale, "a section that was removed", -1, NULL));
    g_assert_true(g_file_set_contents(marker, "an-older-digest", -1, NULL));

    g_assert_true(clawt_skill_install_bundled(dir, &error));
    g_assert_no_error(error);
    g_assert_false(g_file_test(stale, G_FILE_TEST_EXISTS));
    g_assert_true(g_file_test(skill, G_FILE_TEST_IS_REGULAR));

    clawt_test_remove_tree(dir);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/clawtilla-skill/installs-and-loads", test_installs_and_loads);
    g_test_add_func("/clawtilla-skill/implied-for-who-runs-the-fleet",
                    test_implied_for_who_runs_the_fleet);
    g_test_add_func("/clawtilla-skill/chief-keeps-it-without-office",
                    test_chief_keeps_it_without_office);
    g_test_add_func("/clawtilla-skill/upgrade-removes-stale-files",
                    test_upgrade_removes_stale_files);

    return g_test_run();
}
