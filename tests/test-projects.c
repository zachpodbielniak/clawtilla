/*
 * test-projects.c - Projects: config, resolution, and PROJECTS.org
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * - `projects:` read with its defaults, and edited: add, typed set, the
 *   id refused, bad numbers refused, remove;
 * - clawt_project_resolve(): named (and unknown refused), the parent's,
 *   by path, by a shared git remote, and none;
 * - clawt_project_month_start();
 * - the PROJECTS.org region: above the agent's text, replaced in place,
 *   capped, removed, and the agent's own text kept byte for byte.
 */

#include "clawtilla.h"
#include "clawt-test-util.h"

#include <glib/gstdio.h>
#include <string.h>

static void
test_config_projects(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(ClawtConfig) config = clawt_config_load_from_string(
		"projects:\n"
		"  - id: site\n"
		"    name: Website\n"
		"    goal: Ship the new site\n"
		"    directory: /srv/site\n"
		"    agents: [builder, writer]\n"
		"    monthly_budget_usd: 40\n"
		"    budget_hard_stop: true\n"
		"  - name: no id, ignored\n", &error);
	g_autoptr(GPtrArray) projects = NULL;
	g_autoptr(ClawtProjectSpec) site = NULL;
	g_autoptr(ClawtProjectSpec) fresh = NULL;
	g_autofree gchar *brief = NULL;

	g_assert_no_error(error);
	g_test_expect_message("Clawtilla", G_LOG_LEVEL_WARNING,
	                      "*no id, so this project is ignored*");
	projects = clawt_config_get_projects(config);
	g_test_assert_expected_messages();
	g_assert_cmpuint(projects->len, ==, 1);

	site = clawt_config_get_project(config, "site");
	g_assert_cmpstr(site->name, ==, "Website");
	g_assert_cmpstr(site->directory, ==, "/srv/site");
	g_assert_cmpfloat(site->monthly_budget_usd, ==, 40.0);
	g_assert_cmpint(site->budget_warn_percent, ==, 80);
	g_assert_true(site->budget_hard_stop);
	g_assert_true(clawt_project_spec_has_agent(site, "builder"));
	g_assert_false(clawt_project_spec_has_agent(site, "chief"));

	brief = clawt_project_spec_brief(site);
	g_assert_cmpstr(brief, ==, "This task is part of the project Website "
	                           "(site). Its goal: Ship the new site");

	/* edited, typed as the schema says */
	g_assert_true(clawt_config_add_project(config, "docs", &error));
	g_assert_false(clawt_config_add_project(config, "docs", &error));
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_ALREADY_EXISTS);
	g_clear_error(&error);
	g_assert_false(clawt_config_add_project(config, "Bad Id", &error));
	g_clear_error(&error);

	g_assert_true(clawt_config_set_project_value(config, "docs",
	              "monthly_budget_usd", "12.5", &error));
	g_assert_true(clawt_config_set_project_value(config, "docs",
	              "budget_warn_percent", "50", &error));
	g_assert_true(clawt_config_set_project_value(config, "docs",
	              "budget_hard_stop", "yes", &error));
	g_assert_true(clawt_config_set_project_value(config, "docs", "agents",
	              "a, b", &error));
	g_assert_no_error(error);

	fresh = clawt_config_get_project(config, "docs");
	g_assert_cmpfloat(fresh->monthly_budget_usd, ==, 12.5);
	g_assert_cmpint(fresh->budget_warn_percent, ==, 50);
	g_assert_true(fresh->budget_hard_stop);
	g_assert_cmpuint(g_strv_length(fresh->agents), ==, 2);
	g_assert_true(clawt_project_spec_has_agent(fresh, "b"));

	/* the id, a bad amount, a bad percent, an unknown key: refused */
	g_assert_false(clawt_config_set_project_value(config, "docs", "id", "x",
	                                              &error));
	g_clear_error(&error);
	g_assert_false(clawt_config_set_project_value(config, "docs",
	               "monthly_budget_usd", "lots", &error));
	g_clear_error(&error);
	g_assert_false(clawt_config_set_project_value(config, "docs",
	               "budget_warn_percent", "120", &error));
	g_clear_error(&error);
	g_assert_false(clawt_config_set_project_value(config, "docs", "colour",
	                                              "red", &error));
	g_clear_error(&error);
	g_assert_false(clawt_config_set_project_value(config, "ghost", "goal",
	                                              "x", &error));
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND);
	g_clear_error(&error);

	/* emptied removes the key; the default comes back */
	g_assert_true(clawt_config_set_project_value(config, "docs",
	              "budget_warn_percent", "", NULL));
	{
		g_autoptr(ClawtProjectSpec) again = clawt_config_get_project(config,
		                                                             "docs");

		g_assert_cmpint(again->budget_warn_percent, ==, 80);
	}

	g_assert_true(clawt_config_remove_project(config, "docs"));
	g_assert_false(clawt_config_remove_project(config, "docs"));
	g_assert_null(clawt_config_get_project(config, "docs"));
}

/* `git` in @dir with nothing of the user's config. */
static void
git_in(
	const gchar *dir,
	const gchar *args
){
	g_autofree gchar *command = g_strdup_printf(
		"env GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1 "
		"git -C '%s' -c init.defaultBranch=main %s", dir, args);
	gint status = 1;

	g_assert_true(g_spawn_command_line_sync(command, NULL, NULL, &status,
	                                        NULL));
	g_assert_cmpint(status, ==, 0);
}

static void
test_resolve(void)
{
	g_autofree gchar *root = g_dir_make_tmp("clawt-projects-XXXXXX", NULL);
	g_autofree gchar *site = g_build_filename(root, "site", NULL);
	g_autofree gchar *clone = g_build_filename(root, "elsewhere", NULL);
	g_autofree gchar *yaml = NULL;
	g_autoptr(ClawtConfig) config = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *git = g_find_program_in_path("git");
	ClawtProjectSpec *found;

	g_assert_cmpint(g_mkdir_with_parents(site, 0700), ==, 0);
	g_assert_cmpint(g_mkdir_with_parents(clone, 0700), ==, 0);

	yaml = g_strdup_printf(
		"projects:\n"
		"  - id: site\n    directory: \"%s\"\n"
		"  - id: other\n", site);
	config = clawt_config_load_from_string(yaml, &error);
	g_assert_no_error(error);

	/* named, and an unknown name refused rather than guessed */
	found = clawt_project_resolve(config, "other", NULL, NULL, &error);
	g_assert_cmpstr(found->id, ==, "other");
	clawt_project_spec_free(found);
	g_assert_null(clawt_project_resolve(config, "ghost", NULL, NULL,
	                                    &error));
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND);
	g_clear_error(&error);

	/* the parent's, before the repository */
	found = clawt_project_resolve(config, NULL, "other", site, &error);
	g_assert_cmpstr(found->id, ==, "other");
	clawt_project_spec_free(found);

	/* by path, through a trailing slash */
	{
		g_autofree gchar *slashed = g_strconcat(site, "/", NULL);

		found = clawt_project_resolve(config, NULL, NULL, slashed, &error);
		g_assert_nonnull(found);
		g_assert_cmpstr(found->id, ==, "site");
		clawt_project_spec_free(found);
	}

	/* nothing to go on, or nothing matches: no project, no error */
	g_assert_null(clawt_project_resolve(config, NULL, NULL, NULL, &error));
	g_assert_null(clawt_project_resolve(config, NULL, NULL, clone, &error));
	g_assert_no_error(error);

	/* by a remote the two share, spelled differently */
	if (git != NULL) {
		git_in(site, "init -q");
		git_in(site, "remote add origin git@gitlab.com:me/site.git");
		git_in(clone, "init -q");
		git_in(clone, "remote add upstream https://GitLab.com/me/site");

		found = clawt_project_resolve(config, NULL, NULL, clone, &error);
		g_assert_nonnull(found);
		g_assert_cmpstr(found->id, ==, "site");
		clawt_project_spec_free(found);
	}

	clawt_test_remove_tree(root);
}

static void
test_month_start(void)
{
	g_autoptr(GDateTime) mid = g_date_time_new_local(2026, 11, 16, 14, 30, 0);
	g_autoptr(GDateTime) first = g_date_time_new_local(2026, 11, 1, 0, 0, 0);
	g_autoptr(GDateTime) last = g_date_time_new_local(2026, 10, 31, 23, 59,
	                                                  59);
	g_autoptr(GDateTime) october = g_date_time_new_local(2026, 10, 1, 0, 0,
	                                                     0);

	g_assert_cmpint(clawt_project_month_start(g_date_time_to_unix(mid)), ==,
	                g_date_time_to_unix(first));
	g_assert_cmpint(clawt_project_month_start(g_date_time_to_unix(first)),
	                ==, g_date_time_to_unix(first));
	g_assert_cmpint(clawt_project_month_start(g_date_time_to_unix(last)), ==,
	                g_date_time_to_unix(october));
}

/* ── PROJECTS.org ────────────────────────────────────────────────── */

static void
test_projects_region(void)
{
	g_autofree gchar *root = g_dir_make_tmp("clawt-projects-XXXXXX", NULL);
	g_autofree gchar *yaml = g_strdup_printf(
		"agents:\n  - id: a\n    workspace: \"%s\"\n", root);
	g_autoptr(ClawtConfig) config = clawt_config_load_from_string(yaml, NULL);
	ClawtAgentConfig *agent = clawt_config_get_agent(config, "a");
	g_autofree gchar *path = g_build_filename(root, "PROJECTS.org", NULL);
	g_autofree gchar *text = NULL;
	g_autoptr(GString) big = g_string_new(NULL);
	const gchar *own = "#+title: Projects\n\n* Mine\nWhat I keep.\n";
	guint i;

	g_assert_nonnull(agent);
	g_assert_true(g_file_set_contents(path, own, -1, NULL));

	/* first time: after the header, above the agent's own text */
	g_assert_true(clawt_workspace_update_projects(agent,
	              "* Site (~site~)\n\n- Goal :: ship\n\n", NULL));
	g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
	g_assert_true(g_str_has_prefix(text, "#+title: Projects\n"));
	g_assert_nonnull(strstr(text, "# BEGIN clawtilla projects"));
	g_assert_true(strstr(text, "# END clawtilla projects") <
	              strstr(text, "* Mine"));
	g_assert_true(g_str_has_suffix(text, "* Mine\nWhat I keep.\n"));
	g_clear_pointer(&text, g_free);

	/* replaced in place, not added twice */
	g_assert_true(clawt_workspace_update_projects(agent,
	              "* Site (~site~)\n\n- Goal :: ship it\n\n", NULL));
	g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
	g_assert_nonnull(strstr(text, "ship it"));
	g_assert_null(strstr(strstr(text, "# END clawtilla projects") + 1,
	                     "# BEGIN clawtilla projects"));
	g_clear_pointer(&text, g_free);

	/* capped */
	for (i = 0; i < 400; i++)
		g_string_append_printf(big, "- line %u of a long list\n", i);
	g_assert_true(clawt_workspace_update_projects(agent, big->str, NULL));
	g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
	g_assert_cmpuint(strlen(text), <, strlen(own) + 4 * 1024 + 512);
	g_assert_nonnull(strstr(text, "# END clawtilla projects"));
	g_clear_pointer(&text, g_free);

	/* removed: the agent's file is back exactly as it wrote it */
	g_assert_true(clawt_workspace_update_projects(agent, NULL, NULL));
	g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
	g_assert_cmpstr(text, ==, own);

	clawt_test_remove_tree(root);
}

int
main(
	int    argc,
	char **argv
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/projects/config", test_config_projects);
	g_test_add_func("/projects/resolve", test_resolve);
	g_test_add_func("/projects/month-start", test_month_start);
	g_test_add_func("/projects/region", test_projects_region);

	return g_test_run();
}
