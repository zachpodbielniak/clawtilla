/*
 * test-integration.c - Scoping, per-agent identity and what an agent is told
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The failures worth catching here are the ones that only exist between
 * two things: two agents sharing one Matrix login, an inline block and a
 * shared instance both claiming the one channel an agent can have, an
 * override that reaches the file and not the agent.  None of them is
 * visible from one agent, one instance or one getter alone, which is why
 * they were all reachable while every individual piece worked.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>

#include "clawt-test-util.h"

typedef struct {
    gchar       *dir;
    ClawtConfig *config;
} Fixture;

static void
fixture_setup(Fixture *fixture, const gchar *body)
{
    g_autofree gchar *yaml = NULL;
    g_autoptr(GError) error = NULL;

    fixture->dir = g_dir_make_tmp("clawt-int-XXXXXX", NULL);

    yaml = g_strdup_printf(
        "daemon:\n"
        "  state_dir: \"%s/state\"\n"
        "defaults:\n"
        "  workspace_root: \"%s/agents\"\n"
        "%s",
        fixture->dir, fixture->dir, body);

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
agent_named(Fixture *fixture, const gchar *id)
{
    ClawtAgentConfig *agent = clawt_config_get_agent(fixture->config, id);

    g_assert_nonnull(agent);

    return agent;
}

/* ── Scope ───────────────────────────────────────────────────────── */

static const gchar TWO_AGENTS[] =
    "agents:\n"
    "  - id: researcher\n"
    "  - id: scribe\n";

static void
test_scope_all_reaches_everyone(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *yaml = g_strconcat(
        "integrations:\n"
        "  - name: github\n"
        "    type: mcp\n"
        "    scope: all\n"
        "    command: npx\n",
        TWO_AGENTS, NULL);
    ClawtIntegrationConfig *instance;

    fixture_setup(&fixture, yaml);

    instance = clawt_config_get_integration(fixture.config, "github");
    g_assert_nonnull(instance);
    g_assert_cmpint(clawt_integration_config_get_scope(instance), ==,
                    CLAWT_SCOPE_ALL);

    g_assert_true(clawt_integration_config_covers(instance, "researcher"));
    g_assert_true(clawt_integration_config_covers(instance, "scribe"));

    /*
     * Including an agent the file has never heard of.  That is the point
     * of `all`: an agent created tomorrow gets it without anybody
     * revisiting this entry.
     */
    g_assert_true(clawt_integration_config_covers(instance, "not-yet-made"));

    fixture_teardown(&fixture);
}

/*
 * A `teams:` scope reaches the agents on that team.
 *
 * clawt_integration_resolve_for_agent() -- the one function everything
 * downstream goes through -- asked clawt_integration_config_covers(),
 * which forwards team = NULL, and clawt_scope_covers() returns FALSE for
 * a NULL team *before* it looks at the teams list.  So every `teams:`
 * entry matched nobody: the server never reached the agent's .mcp.json,
 * and the daemon's own listing asked the same broken predicate, so both
 * clients reported "not applied" as well.  A configured, documented
 * scope that reaches nobody and says nothing.
 *
 * The variant that takes a team existed the whole time and had no
 * callers at all.
 */
static void
test_scope_teams_reaches_that_team(void)
{
    Fixture fixture = { 0 };
    const gchar *yaml =
        "integrations:\n"
        "  - name: shared-tools\n"
        "    type: mcp\n"
        "    scope: selected\n"
        "    teams: [ops]\n"
        "    command: npx\n"
        "teams:\n"
        "  - id: ops\n"
        "agents:\n"
        "  - id: researcher\n"
        "    team: ops\n"
        "  - id: scribe\n";
    g_autoptr(GPtrArray) covered = NULL;
    g_autoptr(GPtrArray) uncovered = NULL;

    fixture_setup(&fixture, yaml);

    covered = clawt_integration_resolve_for_agent(
        fixture.config, agent_named(&fixture, "researcher"));
    g_assert_nonnull(clawt_integration_find_binding(covered, "mcp"));

    /* And nobody else. */
    uncovered = clawt_integration_resolve_for_agent(
        fixture.config, agent_named(&fixture, "scribe"));
    g_assert_null(clawt_integration_find_binding(uncovered, "mcp"));

    fixture_teardown(&fixture);
}

static void
test_scope_selected_reaches_only_those(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *yaml = g_strconcat(
        "integrations:\n"
        "  - name: github\n"
        "    type: mcp\n"
        "    scope: selected\n"
        "    agents: [researcher]\n"
        "    command: npx\n",
        TWO_AGENTS, NULL);
    ClawtIntegrationConfig *instance;

    fixture_setup(&fixture, yaml);
    instance = clawt_config_get_integration(fixture.config, "github");

    g_assert_true(clawt_integration_config_covers(instance, "researcher"));
    g_assert_false(clawt_integration_config_covers(instance, "scribe"));

    fixture_teardown(&fixture);
}

/*
 * `none` and `enabled: false` are different switches and both have to
 * work on their own -- one is "not yet" and the other is "not now".
 */
static void
test_scope_none_and_disabled_reach_nobody(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *yaml = g_strconcat(
        "integrations:\n"
        "  - name: parked\n"
        "    type: mcp\n"
        "    scope: none\n"
        "    agents: [researcher]\n"
        "    command: npx\n"
        "  - name: off\n"
        "    type: mcp\n"
        "    scope: all\n"
        "    enabled: false\n"
        "    command: npx\n",
        TWO_AGENTS, NULL);

    fixture_setup(&fixture, yaml);

    g_assert_false(clawt_integration_config_covers(
        clawt_config_get_integration(fixture.config, "parked"), "researcher"));
    g_assert_false(clawt_integration_config_covers(
        clawt_config_get_integration(fixture.config, "off"), "researcher"));

    /* The selection survives being parked, so un-parking restores it. */
    {
        g_auto(GStrv) agents = clawt_integration_config_get_agents(
            clawt_config_get_integration(fixture.config, "parked"));

        g_assert_nonnull(agents);
        g_assert_cmpstr(agents[0], ==, "researcher");
    }

    fixture_teardown(&fixture);
}

/*
 * An unrecognised scope reaches nobody rather than everybody.  The two
 * failure modes are not symmetric: a typo that hands a credential to the
 * whole fleet is much worse than one that hands it to nothing.
 */
static void
test_a_misspelt_scope_reaches_nobody(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *yaml = g_strconcat(
        "integrations:\n"
        "  - name: typo\n"
        "    type: mcp\n"
        "    scope: everyone\n"
        "    command: npx\n",
        TWO_AGENTS, NULL);

    fixture_setup(&fixture, yaml);

    g_test_expect_message("Clawtilla", G_LOG_LEVEL_WARNING,
                          "*is not a scope*");
    g_assert_false(clawt_integration_config_covers(
        clawt_config_get_integration(fixture.config, "typo"), "researcher"));
    g_test_assert_expected_messages();

    fixture_teardown(&fixture);
}

/* ── Per-agent overrides ─────────────────────────────────────────── */

static void
test_per_agent_wins_over_the_instance(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *yaml = g_strconcat(
        "integrations:\n"
        "  - name: home\n"
        "    type: matrix\n"
        "    scope: all\n"
        "    homeserver: https://matrix.example.org\n"
        "    user_id: \"@shared:example.org\"\n"
        "    per_agent:\n"
        "      researcher:\n"
        "        user_id: \"@researcher:example.org\"\n",
        TWO_AGENTS, NULL);
    ClawtIntegrationConfig *instance;

    fixture_setup(&fixture, yaml);
    instance = clawt_config_get_integration(fixture.config, "home");

    g_assert_cmpstr(clawt_integration_config_get_string(instance,
                                                        "researcher",
                                                        "user_id"),
                    ==, "@researcher:example.org");

    /* The one without an override falls back. */
    g_assert_cmpstr(clawt_integration_config_get_string(instance, "scribe",
                                                        "user_id"),
                    ==, "@shared:example.org");

    /* And the shared value is still what the instance itself says. */
    g_assert_cmpstr(clawt_integration_config_get_string(instance, NULL,
                                                        "user_id"),
                    ==, "@shared:example.org");

    /* An unrelated key is not shadowed by the presence of an override. */
    g_assert_cmpstr(clawt_integration_config_get_string(instance,
                                                        "researcher",
                                                        "homeserver"),
                    ==, "https://matrix.example.org");

    fixture_teardown(&fixture);
}

/*
 * The failure this prevents is a fleet that looks fine and behaves like
 * one confused person: two agents on one Matrix login receive each
 * other's messages and answer as the same user.
 */
static void
test_a_shared_identity_is_reported(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *yaml = g_strconcat(
        "integrations:\n"
        "  - name: home\n"
        "    type: matrix\n"
        "    scope: all\n"
        "    homeserver: https://matrix.example.org\n"
        "    user_id: \"@shared:example.org\"\n"
        "    access_token: {env: TOKEN}\n",
        TWO_AGENTS, NULL);
    g_autoptr(GPtrArray) warnings = NULL;
    gboolean found = FALSE;
    guint i;

    fixture_setup(&fixture, yaml);

    g_assert_false(clawt_integration_validate_fleet(fixture.config,
                                                    &warnings));

    for (i = 0; i < warnings->len; i++) {
        if (strstr(g_ptr_array_index(warnings, i), "share one user_id"))
            found = TRUE;
    }

    g_assert_true(found);

    fixture_teardown(&fixture);
}

static void
test_separate_identities_are_not_reported(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *yaml = g_strconcat(
        "integrations:\n"
        "  - name: home\n"
        "    type: matrix\n"
        "    scope: all\n"
        "    homeserver: https://matrix.example.org\n"
        "    access_token: {env: TOKEN}\n"
        "    per_agent:\n"
        "      researcher:\n"
        "        user_id: \"@researcher:example.org\"\n"
        "        access_token: {env: R_TOKEN}\n"
        "      scribe:\n"
        "        user_id: \"@scribe:example.org\"\n"
        "        access_token: {env: S_TOKEN}\n",
        TWO_AGENTS, NULL);
    g_autoptr(GPtrArray) warnings = NULL;
    guint i;

    fixture_setup(&fixture, yaml);

    clawt_integration_validate_fleet(fixture.config, &warnings);

    for (i = 0; i < warnings->len; i++)
        g_assert_null(strstr(g_ptr_array_index(warnings, i), "share one"));

    fixture_teardown(&fixture);
}

/* ── Resolution ──────────────────────────────────────────────────── */

/*
 * libreclaw renders one `channels.matrix` block per agent, so a second
 * one has nowhere to go.  Dropping it silently would leave an account
 * that looks configured in the file and receives nothing for ever.
 */
static void
test_an_inline_block_beats_a_shared_instance(void)
{
    Fixture fixture = { 0 };
    const gchar *yaml =
        "integrations:\n"
        "  - name: home\n"
        "    type: matrix\n"
        "    scope: all\n"
        "    homeserver: https://shared.example.org\n"
        "agents:\n"
        "  - id: researcher\n"
        "    integrations:\n"
        "      matrix:\n"
        "        enabled: true\n"
        "        homeserver: https://its-own.example.org\n"
        "        user_id: \"@researcher:example.org\"\n";
    g_autoptr(GPtrArray) bindings = NULL;
    ClawtIntegrationBinding *matrix;

    fixture_setup(&fixture, yaml);

    g_test_expect_message("Clawtilla", G_LOG_LEVEL_WARNING,
                          "*already has a matrix integration*");
    bindings = clawt_integration_resolve_for_agent(
        fixture.config, agent_named(&fixture, "researcher"));
    g_test_assert_expected_messages();

    g_assert_cmpuint(bindings->len, ==, 1);

    matrix = clawt_integration_find_binding(bindings, "matrix");
    g_assert_nonnull(matrix);
    g_assert_false(clawt_integration_binding_is_shared(matrix));
    g_assert_cmpstr(clawt_integration_binding_get_string(matrix,
                                                         "homeserver"),
                    ==, "https://its-own.example.org");

    fixture_teardown(&fixture);
}

/*
 * Several tool servers on one agent is the ordinary case, so `mcp` is
 * deliberately not one-per-agent.
 */
static void
test_several_mcp_servers_coexist(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *yaml = g_strconcat(
        "integrations:\n"
        "  - name: github\n"
        "    type: mcp\n"
        "    scope: all\n"
        "    command: npx\n"
        "  - name: notes\n"
        "    type: mcp\n"
        "    scope: all\n"
        "    command: notes-mcp\n",
        TWO_AGENTS, NULL);
    g_autoptr(GPtrArray) bindings = NULL;

    fixture_setup(&fixture, yaml);

    bindings = clawt_integration_resolve_for_agent(
        fixture.config, agent_named(&fixture, "researcher"));

    g_assert_cmpuint(bindings->len, ==, 2);

    fixture_teardown(&fixture);
}

static void
test_an_mcp_server_is_a_command_or_a_url(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *yaml = g_strconcat(
        "integrations:\n"
        "  - name: neither\n"
        "    type: mcp\n"
        "    scope: all\n"
        "  - name: both\n"
        "    type: mcp\n"
        "    scope: all\n"
        "    command: npx\n"
        "    url: https://example.org/mcp\n",
        TWO_AGENTS, NULL);
    g_autoptr(GPtrArray) bindings = NULL;
    guint i;
    guint refused = 0;

    fixture_setup(&fixture, yaml);

    bindings = clawt_integration_resolve_for_agent(
        fixture.config, agent_named(&fixture, "researcher"));

    for (i = 0; i < bindings->len; i++) {
        g_autoptr(GError) error = NULL;

        if (!clawt_integration_binding_validate(
                g_ptr_array_index(bindings, i), &error)) {
            refused++;
            g_assert_nonnull(error);
        }
    }

    g_assert_cmpuint(refused, ==, 2);

    fixture_teardown(&fixture);
}

/*
 * An unknown type disables one instance and nothing else, so a config
 * written by a newer clawtilla still loads in an older one.
 */
static void
test_an_unknown_type_disables_only_itself(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *yaml = g_strconcat(
        "integrations:\n"
        "  - name: future\n"
        "    type: telepathy\n"
        "    scope: all\n"
        "  - name: github\n"
        "    type: mcp\n"
        "    scope: all\n"
        "    command: npx\n",
        TWO_AGENTS, NULL);
    g_autoptr(GPtrArray) bindings = NULL;

    fixture_setup(&fixture, yaml);

    g_test_expect_message("Clawtilla", G_LOG_LEVEL_WARNING,
                          "*unknown type 'telepathy'*");
    bindings = clawt_integration_resolve_for_agent(
        fixture.config, agent_named(&fixture, "researcher"));
    g_test_assert_expected_messages();

    g_assert_cmpuint(bindings->len, ==, 1);
    g_assert_nonnull(clawt_integration_find_binding(bindings, "mcp"));

    fixture_teardown(&fixture);
}

/* ── Editing ─────────────────────────────────────────────────────── */

static void
test_add_set_and_remove_round_trip(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    ClawtIntegrationConfig *instance;
    static const gchar *const agents[] = { "researcher", NULL };

    fixture_setup(&fixture, TWO_AGENTS);

    instance = clawt_config_add_integration(fixture.config, "home", "matrix",
                                            &error);
    g_assert_no_error(error);
    g_assert_nonnull(instance);

    /* A second one with the same name is refused rather than merged. */
    g_assert_null(clawt_config_add_integration(fixture.config, "home",
                                               "matrix", &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_ALREADY_EXISTS);
    g_clear_error(&error);

    /* A name that could escape a directory is refused outright. */
    g_assert_null(clawt_config_add_integration(fixture.config, "../evil",
                                               "matrix", &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);

    clawt_integration_config_set_string(instance, NULL, "homeserver",
                                        "https://matrix.example.org");
    clawt_integration_config_set_scope(instance,
                                       CLAWT_SCOPE_SELECTED,
                                       agents);
    clawt_integration_config_set_secret(instance, "researcher",
                                        "access_token",
                                        CLAWT_SECRET_BACKEND_ENV, "TOKEN");

    g_assert_true(clawt_integration_config_covers(instance, "researcher"));
    g_assert_false(clawt_integration_config_covers(instance, "scribe"));

    {
        g_autoptr(ClawtSecretRef) ref =
            clawt_integration_config_get_secret(instance, "researcher",
                                                "access_token");

        g_assert_nonnull(ref);
        g_assert_cmpint(clawt_secret_ref_get_backend(ref), ==,
                        CLAWT_SECRET_BACKEND_ENV);
        g_assert_cmpstr(clawt_secret_ref_get_locator(ref), ==, "TOKEN");
    }

    /* It survives being written out and read back. */
    {
        g_autofree gchar *text = clawt_config_to_string(fixture.config);
        g_autoptr(ClawtConfig) reloaded = NULL;
        ClawtIntegrationConfig *again;

        reloaded = clawt_config_load_from_string(text, &error);
        g_assert_no_error(error);

        again = clawt_config_get_integration(reloaded, "home");
        g_assert_nonnull(again);
        g_assert_cmpstr(clawt_integration_config_get_string(again, NULL,
                                                            "homeserver"),
                        ==, "https://matrix.example.org");
        g_assert_true(clawt_integration_config_covers(again, "researcher"));
    }

    g_assert_true(clawt_config_remove_integration(fixture.config, "home"));
    g_assert_null(clawt_config_get_integration(fixture.config, "home"));
    g_assert_false(clawt_config_remove_integration(fixture.config, "home"));

    fixture_teardown(&fixture);
}

/*
 * Clearing an override must take the per-agent block with it when that
 * was the last thing in it -- otherwise every listing shows an agent as
 * having overrides that are not there.
 */
static void
test_clearing_an_override_leaves_nothing_behind(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    ClawtIntegrationConfig *instance;
    g_autofree gchar *text = NULL;

    fixture_setup(&fixture, TWO_AGENTS);

    instance = clawt_config_add_integration(fixture.config, "home", "matrix",
                                            &error);
    g_assert_no_error(error);

    clawt_integration_config_set_string(instance, "researcher", "user_id",
                                        "@researcher:example.org");
    g_assert_true(clawt_integration_config_has_key(instance, "researcher",
                                                   "user_id"));

    clawt_integration_config_set_string(instance, "researcher", "user_id",
                                        NULL);

    text = clawt_config_to_string(fixture.config);
    g_assert_null(strstr(text, "per_agent"));

    fixture_teardown(&fixture);
}

/* ── What the agent is told ──────────────────────────────────────── */

static gchar *
tools_org_of(Fixture *fixture, const gchar *agent_id)
{
    ClawtAgentConfig *agent = agent_named(fixture, agent_id);
    g_autofree gchar *path = clawt_workspace_file_path(agent, "TOOLS.org");
    gchar *text = NULL;

    g_assert_nonnull(path);
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));

    return text;
}

static void
test_tools_org_is_rewritten_between_the_markers(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    ClawtAgentConfig *agent;
    g_autofree gchar *before = NULL;
    g_autofree gchar *after = NULL;

    fixture_setup(&fixture,
        "integrations:\n"
        "  - name: home\n"
        "    type: matrix\n"
        "    scope: all\n"
        "    homeserver: https://matrix.example.org\n"
        "    user_id: \"@agent:example.org\"\n"
        "    rooms: [\"!ops:example.org\"]\n"
        "agents:\n"
        "  - id: researcher\n");

    agent = agent_named(&fixture, "researcher");
    g_assert_true(clawt_workspace_scaffold(agent, &error));
    g_assert_no_error(error);

    /* Something a person wrote, on either side of the managed region. */
    {
        g_autofree gchar *path = clawt_workspace_file_path(agent,
                                                           "TOOLS.org");
        g_autofree gchar *text = NULL;
        g_autofree gchar *edited = NULL;

        g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
        edited = g_strconcat("MY OWN NOTE AT THE TOP\n", text,
                             "\nMY OWN NOTE AT THE BOTTOM\n", NULL);
        g_assert_true(g_file_set_contents(path, edited, -1, NULL));
    }

    before = tools_org_of(&fixture, "researcher");

    g_assert_true(clawt_workspace_update_tools_org(fixture.config, agent,
                                                   &error));
    g_assert_no_error(error);

    after = tools_org_of(&fixture, "researcher");

    /* The integration reached it. */
    g_assert_nonnull(strstr(after, "home"));
    g_assert_nonnull(strstr(after, "!ops:example.org"));
    g_assert_nonnull(strstr(after, "@agent:example.org"));

    /* Both notes survived. */
    g_assert_nonnull(strstr(after, "MY OWN NOTE AT THE TOP"));
    g_assert_nonnull(strstr(after, "MY OWN NOTE AT THE BOTTOM"));

    /* And so did the rest of the file. */
    g_assert_nonnull(strstr(after, "Limits worth knowing"));
    g_assert_cmpstr(before, !=, after);

    /* Exactly one managed region, however many times it runs. */
    g_assert_true(clawt_workspace_update_tools_org(fixture.config, agent,
                                                   &error));

    {
        g_autofree gchar *again = tools_org_of(&fixture, "researcher");
        const gchar *first = strstr(again, "# BEGIN clawtilla integrations");

        g_assert_nonnull(first);
        g_assert_null(strstr(first + 1, "# BEGIN clawtilla integrations"));

        /* Idempotent: a second run changes nothing. */
        g_assert_cmpstr(again, ==, after);
    }

    fixture_teardown(&fixture);
}

/*
 * An agent with nothing is told so, rather than being left with a blank
 * section: an agent that suspects it has an unlisted way of reaching the
 * world will go looking for one.
 */
static void
test_no_integrations_is_said_out_loud(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    ClawtAgentConfig *agent;
    g_autofree gchar *text = NULL;

    fixture_setup(&fixture, "agents:\n  - id: researcher\n");

    agent = agent_named(&fixture, "researcher");
    g_assert_true(clawt_workspace_scaffold(agent, &error));
    g_assert_true(clawt_workspace_update_tools_org(fixture.config, agent,
                                                   &error));
    g_assert_no_error(error);

    text = tools_org_of(&fixture, "researcher");
    g_assert_nonnull(strstr(text, "You have none"));

    fixture_teardown(&fixture);
}

/*
 * A file whose markers somebody removed gets them back, appended.  There
 * is no position in a file of somebody's prose that we could claim to
 * know is the right one, so the end is the only safe answer.
 */
static void
test_missing_markers_are_appended(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    ClawtAgentConfig *agent;
    g_autofree gchar *text = NULL;

    fixture_setup(&fixture,
        "integrations:\n"
        "  - name: github\n"
        "    type: mcp\n"
        "    scope: all\n"
        "    command: npx\n"
        "agents:\n"
        "  - id: researcher\n");

    agent = agent_named(&fixture, "researcher");
    g_assert_true(clawt_workspace_scaffold(agent, &error));

    {
        g_autofree gchar *path = clawt_workspace_file_path(agent,
                                                           "TOOLS.org");

        g_assert_true(g_file_set_contents(path, "Just my own notes.\n", -1,
                                          NULL));
    }

    g_assert_true(clawt_workspace_update_tools_org(fixture.config, agent,
                                                   &error));
    g_assert_no_error(error);

    text = tools_org_of(&fixture, "researcher");
    g_assert_nonnull(strstr(text, "Just my own notes."));
    g_assert_nonnull(strstr(text, "# BEGIN clawtilla integrations"));
    g_assert_nonnull(strstr(text, "clawtilla-github"));

    fixture_teardown(&fixture);
}

/* ── The agent's .mcp.json ───────────────────────────────────────── */

static gchar *
mcp_json_of(Fixture *fixture, const gchar *agent_id)
{
    ClawtAgentConfig *agent = agent_named(fixture, agent_id);
    g_autofree gchar *path = clawt_workspace_file_path(agent, ".mcp.json");
    gchar *text = NULL;

    g_assert_nonnull(path);
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));

    return text;
}

static void
test_an_mcp_integration_reaches_the_agent(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    ClawtAgentConfig *agent;
    g_autofree gchar *text = NULL;

    fixture_setup(&fixture,
        "integrations:\n"
        "  - name: github\n"
        "    type: mcp\n"
        "    scope: all\n"
        "    command: npx\n"
        "    args: [\"-y\", \"@modelcontextprotocol/server-github\"]\n"
        "agents:\n"
        "  - id: researcher\n");

    agent = agent_named(&fixture, "researcher");
    g_assert_true(clawt_workspace_scaffold(agent, &error));
    g_assert_true(clawt_workspace_write_mcp_config(fixture.config, agent,
                                                   "/run/c.sock",
                                                   "/state/researcher",
                                                   &error));
    g_assert_no_error(error);

    text = mcp_json_of(&fixture, "researcher");

    g_assert_nonnull(strstr(text, "clawtilla-github"));
    g_assert_nonnull(strstr(text, "@modelcontextprotocol/server-github"));

    /* clawtilla's own entry is still there beside it. */
    g_assert_nonnull(strstr(text, "\"clawtilla\""));

    fixture_teardown(&fixture);
}

/*
 * An entry left behind after the grant is withdrawn points the agent at
 * a server the fleet has stopped offering, which fails on the first tool
 * call and a long way from the config change that caused it.
 */
static void
test_a_descoped_integration_is_removed(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    ClawtAgentConfig *agent;
    g_autofree gchar *with = NULL;
    g_autofree gchar *without = NULL;

    fixture_setup(&fixture,
        "integrations:\n"
        "  - name: github\n"
        "    type: mcp\n"
        "    scope: all\n"
        "    command: npx\n"
        "agents:\n"
        "  - id: researcher\n");

    agent = agent_named(&fixture, "researcher");
    g_assert_true(clawt_workspace_scaffold(agent, &error));
    g_assert_true(clawt_workspace_write_mcp_config(fixture.config, agent,
                                                   "/run/c.sock",
                                                   "/state/researcher",
                                                   &error));

    with = mcp_json_of(&fixture, "researcher");
    g_assert_nonnull(strstr(with, "clawtilla-github"));

    /* A server of the user's own, which must survive. */
    {
        g_autofree gchar *path = clawt_workspace_file_path(agent,
                                                           ".mcp.json");
        g_autofree gchar *text = NULL;
        g_autofree gchar *edited = NULL;

        g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
        edited = g_strdup_printf(
            "{\n  \"mcpServers\": {\n    \"mine\": {\"command\": \"x\"},\n"
            "    \"clawtilla-github\": {\"command\": \"npx\"}\n  }\n}\n");
        g_assert_true(g_file_set_contents(path, edited, -1, NULL));
    }

    clawt_config_get_integration(fixture.config, "github");
    g_assert_true(clawt_config_remove_integration(fixture.config, "github"));

    g_assert_true(clawt_workspace_write_mcp_config(fixture.config, agent,
                                                   "/run/c.sock",
                                                   "/state/researcher",
                                                   &error));
    g_assert_no_error(error);

    without = mcp_json_of(&fixture, "researcher");

    g_assert_null(strstr(without, "clawtilla-github"));
    g_assert_nonnull(strstr(without, "\"mine\""));

    fixture_teardown(&fixture);
}

/*
 * A secret in an MCP server's environment is a reference in the config
 * and a value in the file the CLI reads.  Anything else means writing a
 * token into clawtilla.yaml, which nothing is allowed to do.
 */
static void
test_an_mcp_env_secret_is_resolved(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    ClawtAgentConfig *agent;
    g_autofree gchar *text = NULL;
    g_autofree gchar *config_text = NULL;

    g_setenv("CLAWT_TEST_GH_TOKEN", "ghp_notreal", TRUE);

    fixture_setup(&fixture,
        "integrations:\n"
        "  - name: github\n"
        "    type: mcp\n"
        "    scope: all\n"
        "    command: npx\n"
        "    env:\n"
        "      GITHUB_TOKEN: {env: CLAWT_TEST_GH_TOKEN}\n"
        "      PLAIN: literal\n"
        "agents:\n"
        "  - id: researcher\n");

    agent = agent_named(&fixture, "researcher");
    g_assert_true(clawt_workspace_scaffold(agent, &error));
    g_assert_true(clawt_workspace_write_mcp_config(fixture.config, agent,
                                                   "/run/c.sock",
                                                   "/state/researcher",
                                                   &error));
    g_assert_no_error(error);

    text = mcp_json_of(&fixture, "researcher");
    g_assert_nonnull(strstr(text, "ghp_notreal"));
    g_assert_nonnull(strstr(text, "literal"));

    /* And the config still holds the reference, not the value. */
    config_text = clawt_config_to_string(fixture.config);
    g_assert_null(strstr(config_text, "ghp_notreal"));

    g_unsetenv("CLAWT_TEST_GH_TOKEN");
    fixture_teardown(&fixture);
}

/* ── The rendered libreclaw config ───────────────────────────────── */

/*
 * The whole point of a shared instance: an account configured once
 * reaches the rendered channel block of every agent in scope, exactly as
 * an inline block would.
 */
static void
test_a_shared_channel_reaches_the_rendered_config(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autofree gchar *researcher = NULL;
    g_autofree gchar *scribe = NULL;

    fixture_setup(&fixture,
        "integrations:\n"
        "  - name: home\n"
        "    type: matrix\n"
        "    scope: selected\n"
        "    agents: [researcher]\n"
        "    homeserver: https://matrix.example.org\n"
        "    user_id: \"@agent:example.org\"\n"
        "    rooms: [\"!ops:example.org\"]\n"
        "agents:\n"
        "  - id: researcher\n"
        "  - id: scribe\n");

    researcher = clawt_config_render_agent(
        fixture.config, agent_named(&fixture, "researcher"),
        "/run/agents.sock", "/state/researcher", &error);
    g_assert_no_error(error);
    g_assert_nonnull(researcher);

    g_assert_nonnull(strstr(researcher, "matrix:"));
    g_assert_nonnull(strstr(researcher, "https://matrix.example.org"));
    g_assert_nonnull(strstr(researcher, "!ops:example.org"));

    scribe = clawt_config_render_agent(
        fixture.config, agent_named(&fixture, "scribe"),
        "/run/agents.sock", "/state/scribe", &error);
    g_assert_no_error(error);

    /* Not in scope, so not in its config at all. */
    g_assert_null(strstr(scribe, "matrix:"));

    fixture_teardown(&fixture);
}


/*
 * A notifier is not a tool the agent can call, and the agent is not
 * involved in one -- but it is still told, because it changes whether
 * saying something is worth doing. Without a branch of its own it
 * rendered a heading and nothing at all.
 */
static void
test_a_notifier_tells_the_agent_what_it_means(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    ClawtAgentConfig *agent;
    g_autofree gchar *text = NULL;

    fixture_setup(&fixture,
        "integrations:\n"
        "  - name: phone\n"
        "    type: notify\n"
        "    scope: all\n"
        "    backend: ntfy\n"
        "    url: https://ntfy.example.org/topic\n"
        "agents:\n"
        "  - id: researcher\n");

    agent = agent_named(&fixture, "researcher");
    g_assert_true(clawt_workspace_scaffold(agent, &error));
    g_assert_true(clawt_workspace_update_tools_org(fixture.config, agent,
                                                   &error));
    g_assert_no_error(error);

    text = tools_org_of(&fixture, "researcher");

    g_assert_nonnull(strstr(text, "phone"));
    g_assert_nonnull(strstr(text, "clawtilla_message_user"));

    /*
     * Both halves: that interrupting works, and that it costs something.
     * Only saying the first would produce an agent that asks about
     * everything.
     */
    g_assert_nonnull(strstr(text, "real option"));
    g_assert_nonnull(strstr(text, "real cost"));

    fixture_teardown(&fixture);
}

/*
 * A Matrix instance with no access_token is refused, rather than
 * producing a config that names a credential file nobody wrote.
 *
 * write_secret_file() returns success when the reference is absent, and
 * says above itself exactly what that costs: "The channel block is
 * rendered pointing at this file regardless, so returning success here
 * produced an agent whose configuration named a credential file that
 * was never written -- it started cleanly and then never
 * authenticated."  The comment describes the failure; the code
 * performed it.
 *
 * The type table already knows access_token is a credential key --
 * clawt_integration_binding_validate() checks exactly that list -- but
 * the check runs from clawt_integration_validate(), which walks only
 * the integrations an agent enables in its own block.  A named
 * instance bound by scope reaches the renderer without passing it.
 *
 * Silence is the whole problem here: an agent that cannot authenticate
 * looks like an agent nobody is talking to.
 */
static void
test_a_matrix_instance_without_a_token_is_refused(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autofree gchar *config_path = NULL;
    gboolean written;

    fixture_setup(&fixture,
        "integrations:\n"
        "  - name: home\n"
        "    type: matrix\n"
        "    scope: all\n"
        "    homeserver: https://matrix.example.org\n"
        "    user_id: \"@agent:example.org\"\n"
        "    rooms: [\"!ops:example.org\"]\n"
        "agents:\n"
        "  - id: researcher\n");

    written = clawt_config_write_agent_files(
        fixture.config, agent_named(&fixture, "researcher"),
        "/run/agents.sock", &config_path, &error);

    g_assert_false(written);
    g_assert_nonnull(error);
    g_assert_nonnull(strstr(error->message, "access_token"));

    fixture_teardown(&fixture);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/integration/scope-all", test_scope_all_reaches_everyone);
    g_test_add_func("/integration/scope-selected",
                    test_scope_selected_reaches_only_those);
    g_test_add_func("/integration/scope-teams",
                    test_scope_teams_reaches_that_team);
    g_test_add_func("/integration/scope-none",
                    test_scope_none_and_disabled_reach_nobody);
    g_test_add_func("/integration/scope-typo",
                    test_a_misspelt_scope_reaches_nobody);
    g_test_add_func("/integration/per-agent",
                    test_per_agent_wins_over_the_instance);
    g_test_add_func("/integration/shared-identity",
                    test_a_shared_identity_is_reported);
    g_test_add_func("/integration/separate-identities",
                    test_separate_identities_are_not_reported);
    g_test_add_func("/integration/inline-wins",
                    test_an_inline_block_beats_a_shared_instance);
    g_test_add_func("/integration/several-mcp",
                    test_several_mcp_servers_coexist);
    g_test_add_func("/integration/mcp-command-or-url",
                    test_an_mcp_server_is_a_command_or_a_url);
    g_test_add_func("/integration/unknown-type",
                    test_an_unknown_type_disables_only_itself);
    g_test_add_func("/integration/round-trip",
                    test_add_set_and_remove_round_trip);
    g_test_add_func("/integration/clearing-an-override",
                    test_clearing_an_override_leaves_nothing_behind);
    g_test_add_func("/integration/tools-org",
                    test_tools_org_is_rewritten_between_the_markers);
    g_test_add_func("/integration/tools-org-empty",
                    test_no_integrations_is_said_out_loud);
    g_test_add_func("/integration/tools-org-no-markers",
                    test_missing_markers_are_appended);
    g_test_add_func("/integration/mcp-json",
                    test_an_mcp_integration_reaches_the_agent);
    g_test_add_func("/integration/mcp-json-descoped",
                    test_a_descoped_integration_is_removed);
    g_test_add_func("/integration/mcp-env-secret",
                    test_an_mcp_env_secret_is_resolved);
    g_test_add_func("/integration/rendered-channel",
                    test_a_shared_channel_reaches_the_rendered_config);
    g_test_add_func("/integration/notify-in-tools-org",
                    test_a_notifier_tells_the_agent_what_it_means);
    g_test_add_func("/integration/matrix-without-a-token-is-refused",
                    test_a_matrix_instance_without_a_token_is_refused);

    return g_test_run();
}
