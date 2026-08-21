/*
 * test-agent-designer.c - Designing an agent by describing it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Driven with AiMockProvider, so the tool loop runs exactly as it would
 * against a real model, with no network, no key and no bill.
 */

#include <clawtilla.h>

/* ai-glib does not expose the mock through its umbrella header. */
#include <agent/ai-mock-provider.h>

static ClawtConfig *
make_config(void)
{
    g_autoptr(GError) error = NULL;
    ClawtConfig *config;

    config = clawt_config_load_from_string("daemon:\n  log_level: info\n",
                                           &error);
    g_assert_no_error(error);

    return config;
}

/* ── The happy path ──────────────────────────────────────────────── */

static void
test_designs_an_agent(void)
{
    g_autoptr(ClawtConfig) config = make_config();
    g_autoptr(ClawtAgentDesigner) designer = clawt_agent_designer_new(config);
    g_autoptr(AiMockProvider) provider = ai_mock_provider_new();
    g_autoptr(GError) error = NULL;
    GHashTable *draft;

    ai_mock_provider_push_tool_use(
        provider, "set_identity",
        "{\"id\":\"researcher\",\"name\":\"Researcher\","
        "\"description\":\"reads code and summarises it\"}");
    ai_mock_provider_push_tool_use(
        provider, "set_model", "{\"model\":\"sonnet\"}");
    ai_mock_provider_push_tool_use(
        provider, "set_computer", "{\"type\":\"container\"}");
    ai_mock_provider_push_tool_use(provider, "commit", "{}");
    ai_mock_provider_push_text(provider, "Designed a research agent.");

    clawt_agent_designer_set_provider(designer, AI_PROVIDER(provider));

    draft = clawt_agent_designer_design(
        designer, "I want something that reads code and summarises it",
        NULL, &error);

    g_assert_no_error(error);
    g_assert_nonnull(draft);

    g_assert_cmpstr(g_hash_table_lookup(draft, "id"), ==, "researcher");
    g_assert_cmpstr(g_hash_table_lookup(draft, "model.model"), ==, "sonnet");
    g_assert_cmpstr(g_hash_table_lookup(draft, "computer.type"), ==,
                    "container");
}

/* Committing goes through the ordinary agent-creation path. */
static void
test_commit_creates_the_agent(void)
{
    g_autoptr(ClawtConfig) config = make_config();
    g_autoptr(ClawtAgentDesigner) designer = clawt_agent_designer_new(config);
    g_autoptr(AiMockProvider) provider = ai_mock_provider_new();
    g_autoptr(GError) error = NULL;
    ClawtAgentConfig *agent;

    ai_mock_provider_push_tool_use(
        provider, "set_identity", "{\"id\":\"writer\",\"name\":\"Writer\"}");
    ai_mock_provider_push_tool_use(provider, "commit", "{}");
    ai_mock_provider_push_text(provider, "Done.");

    clawt_agent_designer_set_provider(designer, AI_PROVIDER(provider));
    g_assert_nonnull(clawt_agent_designer_design(designer, "a writer", NULL,
                                                 &error));

    agent = clawt_agent_designer_commit(designer, &error);

    g_assert_no_error(error);
    g_assert_nonnull(agent);
    g_assert_nonnull(clawt_config_get_agent(config, "writer"));
    g_assert_cmpstr(clawt_agent_config_get_string(agent, "name"), ==,
                    "Writer");
}

/* ── What the model is not allowed to do ─────────────────────────── */

/*
 * The designer has no shell.  ai_tool_executor_new() would have given it
 * bash, read, write and edit, and they cannot be taken back afterwards --
 * so this asserts on the thing that would silently be true if somebody
 * swapped the constructor.
 */
static void
test_the_designer_has_no_shell(void)
{
    g_autoptr(ClawtConfig) config = make_config();
    g_autoptr(ClawtAgentDesigner) designer = clawt_agent_designer_new(config);
    g_autoptr(AiMockProvider) provider = ai_mock_provider_new();
    g_autoptr(GError) error = NULL;

    ai_mock_provider_push_tool_use(provider, "bash",
                                   "{\"command\":\"id > /tmp/clawt-pwned\"}");
    ai_mock_provider_push_tool_use(
        provider, "set_identity", "{\"id\":\"harmless\"}");
    ai_mock_provider_push_tool_use(provider, "commit", "{}");
    ai_mock_provider_push_text(provider, "Done.");

    clawt_agent_designer_set_provider(designer, AI_PROVIDER(provider));
    clawt_agent_designer_design(designer, "anything", NULL, &error);

    g_assert_false(g_file_test("/tmp/clawt-pwned", G_FILE_TEST_EXISTS));
}

/*
 * Unconfined host access is never drafted, however the request is
 * phrased.  Lifting the confinement is the person's decision to make with
 * the documentation in front of them.
 */
static void
test_host_access_is_always_confined(void)
{
    g_autoptr(ClawtConfig) config = make_config();
    g_autoptr(ClawtAgentDesigner) designer = clawt_agent_designer_new(config);
    g_autoptr(AiMockProvider) provider = ai_mock_provider_new();
    g_autoptr(GError) error = NULL;
    GHashTable *draft;

    ai_mock_provider_push_tool_use(
        provider, "set_identity", "{\"id\":\"powerful\"}");
    ai_mock_provider_push_tool_use(
        provider, "set_computer",
        "{\"type\":\"host\",\"confine\":\"none\"}");
    ai_mock_provider_push_tool_use(provider, "commit", "{}");
    ai_mock_provider_push_text(provider, "Done.");

    clawt_agent_designer_set_provider(designer, AI_PROVIDER(provider));
    draft = clawt_agent_designer_design(designer,
                                        "give it full access to my machine",
                                        NULL, &error);

    g_assert_no_error(error);
    g_assert_cmpstr(g_hash_table_lookup(draft, "computer.host.confine"), ==,
                    "workspace");
    g_assert_cmpstr(
        g_hash_table_lookup(draft, "computer.host.confirm_host_control"),
        ==, "true");
}

/* An id that is already taken is refused, with a reason the model can act on. */
static void
test_duplicate_id_is_refused(void)
{
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(ClawtAgentDesigner) designer = NULL;
    g_autoptr(AiMockProvider) provider = ai_mock_provider_new();
    g_autoptr(GError) error = NULL;
    GHashTable *draft;

    config = clawt_config_load_from_string("agents:\n  - id: chief\n",
                                           &error);
    g_assert_no_error(error);

    designer = clawt_agent_designer_new(config);

    /* First attempt collides; the model is told and picks another. */
    ai_mock_provider_push_tool_use(provider, "set_identity",
                                   "{\"id\":\"chief\"}");
    ai_mock_provider_push_tool_use(provider, "set_identity",
                                   "{\"id\":\"chief-two\"}");
    ai_mock_provider_push_tool_use(provider, "commit", "{}");
    ai_mock_provider_push_text(provider, "Done.");

    clawt_agent_designer_set_provider(designer, AI_PROVIDER(provider));
    draft = clawt_agent_designer_design(designer, "another chief", NULL,
                                        &error);

    g_assert_no_error(error);
    g_assert_cmpstr(g_hash_table_lookup(draft, "id"), ==, "chief-two");
}

/* A malformed id is refused with the rule stated. */
static void
test_a_bad_id_is_refused(void)
{
    g_autoptr(ClawtConfig) config = make_config();
    g_autoptr(ClawtAgentDesigner) designer = clawt_agent_designer_new(config);
    g_autoptr(AiMockProvider) provider = ai_mock_provider_new();
    g_autoptr(GError) error = NULL;

    ai_mock_provider_push_tool_use(provider, "set_identity",
                                   "{\"id\":\"my agent!\"}");
    ai_mock_provider_push_text(provider, "I could not name it.");

    clawt_agent_designer_set_provider(designer, AI_PROVIDER(provider));

    g_assert_null(clawt_agent_designer_design(designer, "anything", NULL,
                                              &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_AI);
}

/*
 * A model that talks without drafting anything produces an error rather
 * than an empty draft that fails confusingly later.
 */
static void
test_a_model_that_never_drafts_is_an_error(void)
{
    g_autoptr(ClawtConfig) config = make_config();
    g_autoptr(ClawtAgentDesigner) designer = clawt_agent_designer_new(config);
    g_autoptr(AiMockProvider) provider = ai_mock_provider_new();
    g_autoptr(GError) error = NULL;

    ai_mock_provider_push_text(provider, "What would you like it to do?");

    clawt_agent_designer_set_provider(designer, AI_PROVIDER(provider));

    g_assert_null(clawt_agent_designer_design(designer, "something", NULL,
                                              &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_AI);
    g_assert_nonnull(strstr(error->message, "set_identity"));
}

/* Without a provider the failure names the config key that fixes it. */
static void
test_no_provider_says_what_to_set(void)
{
    g_autoptr(ClawtConfig) config = make_config();
    g_autoptr(ClawtAgentDesigner) designer = clawt_agent_designer_new(config);
    g_autoptr(GError) error = NULL;

    g_assert_null(clawt_agent_designer_design(designer, "anything", NULL,
                                              &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_AI);
    g_assert_nonnull(strstr(error->message, "ai_assist.provider"));
}

/*
 * An agent drafted with an integration but no settings is refused at
 * commit and rolled back.  An agent that exists but cannot start is worse
 * than one that was never added: it shows up in every listing looking
 * real.
 */
static void
test_an_invalid_commit_rolls_back(void)
{
    g_autoptr(ClawtConfig) config = make_config();
    g_autoptr(ClawtAgentDesigner) designer = clawt_agent_designer_new(config);
    g_autoptr(AiMockProvider) provider = ai_mock_provider_new();
    g_autoptr(GError) error = NULL;

    ai_mock_provider_push_tool_use(provider, "set_identity",
                                   "{\"id\":\"chatty\"}");
    ai_mock_provider_push_tool_use(provider, "add_integration",
                                   "{\"integration\":\"matrix\"}");
    ai_mock_provider_push_tool_use(provider, "commit", "{}");
    ai_mock_provider_push_text(provider, "Done.");

    clawt_agent_designer_set_provider(designer, AI_PROVIDER(provider));
    g_assert_nonnull(clawt_agent_designer_design(designer, "a chat agent",
                                                 NULL, &error));

    g_assert_null(clawt_agent_designer_commit(designer, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID);

    /* Rolled back, not half-created. */
    g_assert_null(clawt_config_get_agent(config, "chatty"));
}

/* An unknown integration is refused rather than written through. */
static void
test_unknown_integration_is_refused(void)
{
    g_autoptr(ClawtConfig) config = make_config();
    g_autoptr(ClawtAgentDesigner) designer = clawt_agent_designer_new(config);
    g_autoptr(AiMockProvider) provider = ai_mock_provider_new();
    g_autoptr(GError) error = NULL;
    GHashTable *draft;

    ai_mock_provider_push_tool_use(provider, "set_identity",
                                   "{\"id\":\"agent\"}");
    ai_mock_provider_push_tool_use(provider, "add_integration",
                                   "{\"integration\":\"telepathy\"}");
    ai_mock_provider_push_tool_use(provider, "commit", "{}");
    ai_mock_provider_push_text(provider, "Done.");

    clawt_agent_designer_set_provider(designer, AI_PROVIDER(provider));
    draft = clawt_agent_designer_design(designer, "anything", NULL, &error);

    g_assert_no_error(error);
    g_assert_null(g_hash_table_lookup(draft,
                                      "integrations.telepathy.enabled"));
}

/* The preview is readable YAML naming the agent. */
static void
test_preview_shows_the_draft(void)
{
    g_autoptr(ClawtConfig) config = make_config();
    g_autoptr(ClawtAgentDesigner) designer = clawt_agent_designer_new(config);
    g_autoptr(AiMockProvider) provider = ai_mock_provider_new();
    g_autofree gchar *empty = NULL;
    g_autofree gchar *preview = NULL;
    g_autoptr(GError) error = NULL;

    empty = clawt_agent_designer_preview(designer);
    g_assert_nonnull(strstr(empty, "Nothing has been drafted"));

    ai_mock_provider_push_tool_use(
        provider, "set_identity",
        "{\"id\":\"researcher\",\"description\":\"reads things\"}");
    ai_mock_provider_push_tool_use(provider, "commit", "{}");
    ai_mock_provider_push_text(provider, "Done.");

    clawt_agent_designer_set_provider(designer, AI_PROVIDER(provider));
    clawt_agent_designer_design(designer, "a reader", NULL, &error);

    preview = clawt_agent_designer_preview(designer);

    g_assert_nonnull(strstr(preview, "agents:"));
    g_assert_nonnull(strstr(preview, "researcher"));
    g_assert_nonnull(strstr(preview, "reads things"));
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/designer/designs", test_designs_an_agent);
    g_test_add_func("/designer/commit", test_commit_creates_the_agent);

    g_test_add_func("/designer/no-shell", test_the_designer_has_no_shell);
    g_test_add_func("/designer/host-confined",
                    test_host_access_is_always_confined);
    g_test_add_func("/designer/duplicate-id", test_duplicate_id_is_refused);
    g_test_add_func("/designer/bad-id", test_a_bad_id_is_refused);
    g_test_add_func("/designer/never-drafts",
                    test_a_model_that_never_drafts_is_an_error);
    g_test_add_func("/designer/no-provider",
                    test_no_provider_says_what_to_set);
    g_test_add_func("/designer/invalid-commit-rolls-back",
                    test_an_invalid_commit_rolls_back);
    g_test_add_func("/designer/unknown-integration",
                    test_unknown_integration_is_refused);
    g_test_add_func("/designer/preview", test_preview_shows_the_draft);

    return g_test_run();
}
