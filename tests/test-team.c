/*
 * test-team.c - Who may hand work to whom
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"

#include <string.h>

#include "clawt-test-util.h"

static ClawtConfig *
fleet(const gchar *yaml)
{
    g_autoptr(GError) error = NULL;
    ClawtConfig *config = clawt_config_load_from_string(yaml, &error);

    g_assert_no_error(error);
    g_assert_nonnull(config);

    return config;
}

/*
 * The chief of staff is the lead of every team, so it may assign
 * anywhere. Dividing work across teams is the whole of the role, and
 * confining it to one would leave nobody able to do it.
 */
static void
test_the_chief_may_assign_anywhere(void)
{
    g_autoptr(ClawtConfig) config = fleet(
        "teams:\n  - id: research\n  - id: build\n"
        "agents:\n"
        "  - id: chief\n    chief_of_staff: true\n"
        "  - id: reader\n    team: research\n"
        "  - id: builder\n    team: build\n");
    g_autofree gchar *reason = NULL;

    g_assert_true(clawt_team_may_assign(
        clawt_config_get_agent(config, "chief"),
        clawt_config_get_agent(config, "reader"), &reason));
    g_assert_null(reason);

    g_assert_true(clawt_team_may_assign(
        clawt_config_get_agent(config, "chief"),
        clawt_config_get_agent(config, "builder"), NULL));
}

/* A lead assigns within its own team... */
static void
test_a_lead_assigns_inside_its_team(void)
{
    g_autoptr(ClawtConfig) config = fleet(
        "teams:\n  - id: research\n"
        "agents:\n"
        "  - id: boss\n    team: research\n    team_role: lead\n"
        "  - id: reader\n    team: research\n");

    g_assert_true(clawt_team_may_assign(
        clawt_config_get_agent(config, "boss"),
        clawt_config_get_agent(config, "reader"), NULL));
}

/* ...and nowhere else, with somewhere to send it instead. */
static void
test_a_lead_cannot_reach_another_team(void)
{
    g_autoptr(ClawtConfig) config = fleet(
        "teams:\n  - id: research\n  - id: build\n"
        "agents:\n"
        "  - id: boss\n    team: research\n    team_role: lead\n"
        "  - id: builder\n    team: build\n");
    g_autofree gchar *reason = NULL;

    g_assert_false(clawt_team_may_assign(
        clawt_config_get_agent(config, "boss"),
        clawt_config_get_agent(config, "builder"), &reason));

    g_assert_nonnull(reason);
    g_assert_nonnull(strstr(reason, "not on your team"));

    /*
     * The refusal has to say what to do instead, or the agent tries the
     * same thing again in a different shape.
     */
    g_assert_nonnull(strstr(reason, "chief of staff"));
}

/*
 * A member assigns to nobody -- and is told that talking is not
 * restricted, because the difference is the whole point of the two roles
 * and an agent that reads "no" without the rest concludes it cannot
 * collaborate either.
 */
static void
test_a_member_assigns_to_nobody(void)
{
    g_autoptr(ClawtConfig) config = fleet(
        "teams:\n  - id: research\n"
        "agents:\n"
        "  - id: one\n    team: research\n"
        "  - id: two\n    team: research\n");
    g_autofree gchar *reason = NULL;

    g_assert_false(clawt_team_may_assign(
        clawt_config_get_agent(config, "one"),
        clawt_config_get_agent(config, "two"), &reason));

    g_assert_nonnull(reason);
    g_assert_nonnull(strstr(reason, "not its lead"));
    g_assert_nonnull(strstr(reason, "message"));
}

/* A lead with no team leads nobody, and is told so rather than refused
 * with something that sounds like the other agent's fault. */
static void
test_a_lead_with_no_team_is_told_why(void)
{
    g_autoptr(ClawtConfig) config = fleet(
        "agents:\n"
        "  - id: boss\n    team_role: lead\n"
        "  - id: other\n");
    g_autofree gchar *reason = NULL;

    g_assert_false(clawt_team_may_assign(
        clawt_config_get_agent(config, "boss"),
        clawt_config_get_agent(config, "other"), &reason));

    g_assert_nonnull(strstr(reason, "belong to no "));
}

/* Delegating to yourself is a no-op with extra steps. */
static void
test_nobody_assigns_to_themselves(void)
{
    g_autoptr(ClawtConfig) config = fleet(
        "agents:\n  - id: chief\n    chief_of_staff: true\n");
    g_autofree gchar *reason = NULL;

    g_assert_false(clawt_team_may_assign(
        clawt_config_get_agent(config, "chief"),
        clawt_config_get_agent(config, "chief"), &reason));

    g_assert_nonnull(strstr(reason, "that is you"));
}

/*
 * A fleet with no teams at all behaves as it did before there were any:
 * nothing is wrong with it, and nobody but the chief assigns.
 */
static void
test_a_fleet_without_teams_is_valid(void)
{
    g_autoptr(ClawtConfig) config = fleet(
        "agents:\n  - id: alpha\n  - id: beta\n");
    g_auto(GStrv) warnings = NULL;

    g_assert_true(clawt_team_validate_fleet(config, &warnings));
    g_assert_null(warnings);
}

/*
 * Two leads on one team is not picked between. Which of them may assign
 * is exactly the question, and answering it by file order is a decision
 * nobody can see being made.
 */
static void
test_two_leads_on_one_team_is_reported(void)
{
    g_autoptr(ClawtConfig) config = fleet(
        "teams:\n  - id: research\n"
        "agents:\n"
        "  - id: one\n    team: research\n    team_role: lead\n"
        "  - id: two\n    team: research\n    team_role: lead\n");
    g_auto(GStrv) warnings = NULL;

    g_assert_false(clawt_team_validate_fleet(config, &warnings));
    g_assert_nonnull(warnings);
    g_assert_nonnull(strstr(warnings[0], "two leads"));
    g_assert_nonnull(strstr(warnings[0], "research"));
}

/* An agent on a team nobody declared is usually a typo. */
static void
test_an_undeclared_team_is_reported(void)
{
    g_autoptr(ClawtConfig) config = fleet(
        "teams:\n  - id: research\n"
        "agents:\n  - id: one\n    team: reserch\n");
    g_auto(GStrv) warnings = NULL;

    g_assert_false(clawt_team_validate_fleet(config, &warnings));
    g_assert_nonnull(strstr(warnings[0], "not declared"));
}

/* A team with members and no lead still works, and is worth saying. */
static void
test_a_team_with_no_lead_is_reported(void)
{
    g_autoptr(ClawtConfig) config = fleet(
        "teams:\n  - id: research\n"
        "agents:\n  - id: one\n    team: research\n");
    g_auto(GStrv) warnings = NULL;

    g_assert_false(clawt_team_validate_fleet(config, &warnings));
    g_assert_nonnull(strstr(warnings[0], "no lead"));
    g_assert_nonnull(strstr(warnings[0], "chief of staff"));
}

/* A declared team nobody has joined yet is not a problem. */
static void
test_an_empty_team_is_not_a_warning(void)
{
    g_autoptr(ClawtConfig) config = fleet(
        "teams:\n  - id: research\n"
        "agents:\n  - id: one\n");
    g_auto(GStrv) warnings = NULL;

    g_assert_true(clawt_team_validate_fleet(config, &warnings));
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/team/chief-assigns-anywhere",
                    test_the_chief_may_assign_anywhere);
    g_test_add_func("/team/lead-assigns-inside",
                    test_a_lead_assigns_inside_its_team);
    g_test_add_func("/team/lead-stops-at-the-team-edge",
                    test_a_lead_cannot_reach_another_team);
    g_test_add_func("/team/member-assigns-to-nobody",
                    test_a_member_assigns_to_nobody);
    g_test_add_func("/team/lead-with-no-team",
                    test_a_lead_with_no_team_is_told_why);
    g_test_add_func("/team/no-self-assignment",
                    test_nobody_assigns_to_themselves);
    g_test_add_func("/team/fleet-without-teams-is-valid",
                    test_a_fleet_without_teams_is_valid);
    g_test_add_func("/team/two-leads-reported",
                    test_two_leads_on_one_team_is_reported);
    g_test_add_func("/team/undeclared-team-reported",
                    test_an_undeclared_team_is_reported);
    g_test_add_func("/team/no-lead-reported",
                    test_a_team_with_no_lead_is_reported);
    g_test_add_func("/team/empty-team-is-fine",
                    test_an_empty_team_is_not_a_warning);

    return g_test_run();
}
