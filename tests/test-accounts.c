/*
 * test-accounts.c - Agent CLI accounts
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * An account is a directory an agent's CLI keeps its login in.  These
 * tests pin the three ways it must not quietly become the default login:
 * an unknown id, an account for a different CLI, and a CLI that cannot
 * take one at all.  The render test reads the rendered file back with
 * libreclaw's own parser, because the producer agreeing with itself says
 * nothing about whether the consumer reads the same key.
 */

#include "clawtilla.h"
#include "clawt-test-util.h"

#include <glib/gstdio.h>
#include <string.h>
#include <libreclaw.h>

static ClawtAgentConfig *
first_agent(ClawtConfig *config)
{
    GPtrArray *agents = clawt_config_get_agents(config);

    g_assert_cmpuint(agents->len, >, 0);
    return g_ptr_array_index(agents, 0);
}

static ClawtConfig *
load(const gchar *agent_yaml)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *yaml = g_strdup_printf(
        "accounts:\n"
        "  - id: work\n"
        "    provider: claude-code\n"
        "    config_dir: /acct/claude-work\n"
        "  - id: codex-home\n"
        "    provider: codex-cli\n"
        "    config_dir: ~/codex-home\n"
        "  - id: cursor-two\n"
        "    provider: cursor\n"
        "    config_dir: /acct/cursor\n"
        "agents:\n"
        "%s", agent_yaml);
    ClawtConfig *config = clawt_config_load_from_string(yaml, &error);

    g_assert_no_error(error);
    return config;
}

/* The account renders as ai.config_dir, and libreclaw reads it back. */
static void
test_account_renders_and_round_trips(void)
{
    g_autoptr(ClawtConfig) config = load(
        "  - id: a\n"
        "    model:\n"
        "      provider: claude-code\n"
        "      account: work\n");
    ClawtAgentConfig *agent = first_agent(config);
    g_autofree gchar *rendered = NULL;
    g_autofree gchar *dir = g_dir_make_tmp("clawt-accounts-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(dir, "config.yaml", NULL);
    g_autoptr(LcConfig) parsed = lc_config_new();
    g_autoptr(GError) error = NULL;
    GPtrArray *providers;

    g_assert_false(clawt_agent_config_is_shadow(agent));
    rendered = clawt_config_render_agent(config, agent, "/tmp/s.sock", "/tmp/state", &error);
    g_assert_no_error(error);
    g_assert_nonnull(strstr(rendered, "config_dir: \"/acct/claude-work\""));

    g_assert_true(g_file_set_contents(path, rendered, -1, &error));
    g_assert_true(lc_config_load_from_path(parsed, path, &error));
    g_assert_no_error(error);
    providers = lc_config_get_ai_providers(parsed);
    g_assert_nonnull(providers);
    g_assert_cmpstr(lc_provider_config_get_config_dir(g_ptr_array_index(providers, 0)),
                    ==, "/acct/claude-work");

    clawt_test_remove_tree(dir);
}

/* ~ is expanded, so the CLI is not handed a literal tilde. */
static void
test_account_expands_home(void)
{
    g_autoptr(ClawtConfig) config = load("  - id: a\n");
    g_autoptr(ClawtAccountSpec) account = clawt_config_get_account(config, "codex-home");
    g_autofree gchar *expected = g_build_filename(g_get_home_dir(), "codex-home", NULL);

    g_assert_nonnull(account);
    g_assert_cmpstr(account->config_dir, ==, expected);
}

/* Without an account the agent's config says nothing, and the CLI's own login runs. */
static void
test_no_account_renders_nothing(void)
{
    g_autoptr(ClawtConfig) config = load("  - id: a\n");
    g_autofree gchar *rendered = clawt_config_render_agent(
        config, first_agent(config), "/tmp/s.sock", "/tmp/state", NULL);

    g_assert_null(strstr(rendered, "config_dir:"));
}

static void
assert_shadow(const gchar *agent_yaml, const gchar *reason_part)
{
    g_autoptr(ClawtConfig) config = load(agent_yaml);
    ClawtAgentConfig *agent = first_agent(config);

    g_assert_true(clawt_agent_config_is_shadow(agent));
    g_assert_nonnull(strstr(clawt_agent_config_get_shadow_reason(agent), reason_part));
}

static void
test_unknown_account_is_shadow(void)
{
    assert_shadow("  - id: a\n    model:\n      account: nobody\n", "no account 'nobody'");
}

/* A Claude login on a codex agent would be CODEX_HOME pointed at ~/.claude. */
static void
test_wrong_cli_is_shadow(void)
{
    assert_shadow("  - id: a\n"
                  "    model:\n"
                  "      provider: codex-cli\n"
                  "      account: work\n",
                  "is a claude-code login and this agent runs codex-cli");
}

/* cursor keys its login on $HOME; the answer comes from the client itself. */
static void
test_home_keyed_cli_is_shadow(void)
{
    assert_shadow("  - id: a\n"
                  "    model:\n"
                  "      provider: cursor\n"
                  "      account: cursor-two\n",
                  "keeps its login under $HOME");
}

gint
main(gint argc, gchar *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/accounts/renders-and-round-trips", test_account_renders_and_round_trips);
    g_test_add_func("/accounts/expands-home", test_account_expands_home);
    g_test_add_func("/accounts/none-renders-nothing", test_no_account_renders_nothing);
    g_test_add_func("/accounts/unknown-is-shadow", test_unknown_account_is_shadow);
    g_test_add_func("/accounts/wrong-cli-is-shadow", test_wrong_cli_is_shadow);
    g_test_add_func("/accounts/home-keyed-cli-is-shadow", test_home_keyed_cli_is_shadow);

    return g_test_run();
}
