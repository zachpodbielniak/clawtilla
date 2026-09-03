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

/* ── What a client draws beside the name ───────────────────────── */

/*
 * The chief wins, and never draws both.
 *
 * A chief of staff is the lead of every team, so an agent that is both
 * is completely described by the stronger of the two -- and it is
 * ordinarily both, because `team_role` defaults to `member` only for
 * agents nobody has given a standing.
 */
static void
test_the_chief_outranks_a_lead(void)
{
    g_assert_cmpint(clawt_team_badge_for(TRUE, "lead"), ==,
                    CLAWT_TEAM_BADGE_CHIEF);
    g_assert_cmpint(clawt_team_badge_for(TRUE, "member"), ==,
                    CLAWT_TEAM_BADGE_CHIEF);
    g_assert_cmpint(clawt_team_badge_for(TRUE, NULL), ==,
                    CLAWT_TEAM_BADGE_CHIEF);
}

static void
test_a_lead_is_marked_and_a_member_is_not(void)
{
    g_assert_cmpint(clawt_team_badge_for(FALSE, "lead"), ==,
                    CLAWT_TEAM_BADGE_LEAD);
    g_assert_cmpint(clawt_team_badge_for(FALSE, "member"), ==,
                    CLAWT_TEAM_BADGE_NONE);
}

/*
 * Every nick the type carries is classified, walked out of the enum
 * rather than listed here.
 *
 * The point is the *absence* of a fallthrough: a standing added to
 * ClawtTeamRole and not to this function would be silently unmarked in
 * both clients, and a badge nobody draws looks exactly like an agent
 * that does not have the role -- which is how `team_role` came to be in
 * the daemon's reply for a year with neither client drawing it.
 */
static void
test_every_role_the_enum_carries_is_classified(void)
{
    g_autoptr(GEnumClass) klass = g_type_class_ref(CLAWT_TYPE_TEAM_ROLE);
    guint i;

    for (i = 0; i < klass->n_values; i++) {
        const gchar *nick = klass->values[i].value_nick;
        ClawtTeamBadge badge = clawt_team_badge_for(FALSE, nick);

        /*
         * `member` is the one role that is deliberately unmarked, so it
         * is named rather than exempted by a rule that would also
         * exempt the next role somebody forgets.
         */
        if (g_strcmp0(nick, "member") == 0)
            g_assert_cmpint(badge, ==, CLAWT_TEAM_BADGE_NONE);
        else
            g_assert_cmpint(badge, !=, CLAWT_TEAM_BADGE_NONE);
    }
}

/*
 * A nick the type does not have claims nothing.
 *
 * It reaches here from a daemon newer than the client, and drawing a
 * standing on an agent whose standing this build cannot read would be
 * a confident wrong answer about who may hand out work.  Resolved
 * through the enum rather than compared against a spelled-out "lead"
 * for the same reason: every hand-written copy of an option's values
 * in this tree has drifted, and one of them drew every completed task
 * grey for months.
 */
static void
test_a_role_this_build_cannot_read_claims_nothing(void)
{
    g_assert_cmpint(clawt_team_badge_for(FALSE, "principal"), ==,
                    CLAWT_TEAM_BADGE_NONE);
    g_assert_cmpint(clawt_team_badge_for(FALSE, ""), ==,
                    CLAWT_TEAM_BADGE_NONE);
    g_assert_cmpint(clawt_team_badge_for(FALSE, NULL), ==,
                    CLAWT_TEAM_BADGE_NONE);
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

    g_test_add_func("/team/chief-outranks-a-lead",
                    test_the_chief_outranks_a_lead);
    g_test_add_func("/team/lead-marked-member-not",
                    test_a_lead_is_marked_and_a_member_is_not);
    g_test_add_func("/team/every-role-is-classified",
                    test_every_role_the_enum_carries_is_classified);
    g_test_add_func("/team/unknown-role-claims-nothing",
                    test_a_role_this_build_cannot_read_claims_nothing);

    return g_test_run();
}
