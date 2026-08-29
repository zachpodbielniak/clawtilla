/*
 * test-model-catalog.c - Which providers exist, and what each is for
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include <clawtilla.h>

#include <string.h>

#include "clawt-test-util.h"

/*
 * The `agent` column is checked against libreclaw rather than against a
 * list written down here twice.
 *
 * lc_provider_type_normalize() is the function that decides what an
 * agent actually runs on, and it rewrites anything it does not
 * recognise to claude-code with a warning. That is why this matters:
 * clawtilla offered "openai" as an agent's provider for a while, and
 * picking it did not run OpenAI -- it ran Claude Code and handed it
 * "gpt-4o" as a model name. Pinning the column to the function means
 * the day libreclaw gains or loses a backend, this fails instead of
 * clawtilla quietly lying about it.
 */
static void
test_agent_providers_are_the_ones_libreclaw_runs(void)
{
    const ClawtProviderInfo *catalog;
    gsize n_providers = 0;
    gsize i;
    gboolean saw_one = FALSE;

    catalog = clawt_model_catalog_get(&n_providers);
    g_assert_cmpuint(n_providers, >, 0);

    for (i = 0; i < n_providers; i++) {
        const gchar *resolved;

        if (!catalog[i].agent)
            continue;

        saw_one = TRUE;

        /*
         * Recognised means normalize hands the id straight back. A
         * provider it has to translate is one whose spelling clawtilla
         * has got wrong.
         */
        resolved = lc_provider_type_normalize(catalog[i].id, "test");
        g_assert_cmpstr(resolved, ==, catalog[i].id);
    }

    g_assert_true(saw_one);
}

/*
 * And the other direction: an HTTP provider must not be offered as
 * something an agent runs on, however good the model behind it is.
 */
static void
test_http_providers_are_not_offered_as_agents(void)
{
    const ClawtProviderInfo *catalog;
    gsize n_providers = 0;
    gsize i;

    catalog = clawt_model_catalog_get(&n_providers);

    for (i = 0; i < n_providers; i++) {
        if (!catalog[i].tools)
            continue;

        /*
         * The two flags are very nearly exclusive: tools means ai-glib
         * drives it over HTTP, and libreclaw drives nothing over HTTP.
         */
        g_assert_false(catalog[i].agent);
    }
}

/*
 * Every provider libreclaw can run an agent on is offered.
 *
 * grok-build -- xAI's grok CLI in headless mode -- was missing for a
 * while, so it could not be chosen at all even though libreclaw has
 * always known how to drive it.
 */
static void
test_every_libreclaw_backend_is_offered(void)
{
    static const gchar *backends[] = {
        "claude-code", "claude-tmux", "opencode", "grok-build"
    };
    const ClawtProviderInfo *catalog;
    gsize n_providers = 0;
    gsize i;
    gsize j;

    catalog = clawt_model_catalog_get(&n_providers);

    for (i = 0; i < G_N_ELEMENTS(backends); i++) {
        gboolean found = FALSE;

        for (j = 0; j < n_providers; j++) {
            if (g_strcmp0(catalog[j].id, backends[i]) != 0)
                continue;

            g_assert_true(catalog[j].agent);
            found = TRUE;
            break;
        }

        if (!found)
            g_error("no catalogue entry for libreclaw backend '%s'",
                    backends[i]);
    }
}

/*
 * The designer needs a provider that takes tool definitions, and the
 * default in the schema has to be one of them.
 *
 * It was claude-code, which is a CLI backend: ai-glib's CLI clients
 * drop the tool list, so the shipped default refused the only job it
 * existed to do.
 */
static void
test_designer_default_can_take_tools(void)
{
    const ClawtSchemaEntry *entry;
    const ClawtProviderInfo *provider;
    const gchar *configured;

    entry = clawt_config_schema_lookup("ai_assist.provider");
    g_assert_nonnull(entry);
    configured = entry->default_value;
    g_assert_nonnull(configured);

    provider = clawt_model_catalog_find_provider(configured);
    g_assert_nonnull(provider);
    g_assert_true(provider->tools);
}

/*
 * And the default agent provider has to be one an agent can run on.
 */
static void
test_agent_default_can_be_an_agent(void)
{
    const ClawtSchemaEntry *entry;
    const ClawtProviderInfo *provider;
    const gchar *configured;

    entry = clawt_config_schema_lookup("defaults.provider");
    g_assert_nonnull(entry);
    configured = entry->default_value;
    g_assert_nonnull(configured);

    provider = clawt_model_catalog_find_provider(configured);
    g_assert_nonnull(provider);
    g_assert_true(provider->agent);

    /* And the catalogue's own idea of a default agrees with the schema. */
    g_assert_cmpstr(clawt_model_catalog_default_provider(), ==, configured);
}

/*
 * Every provider that lists models lists at least one, and no entry is
 * missing the fields a client reads unconditionally.
 */
static void
test_entries_are_complete(void)
{
    const ClawtProviderInfo *catalog;
    gsize n_providers = 0;
    gsize i;

    catalog = clawt_model_catalog_get(&n_providers);

    for (i = 0; i < n_providers; i++) {
        gsize j;

        g_assert_nonnull(catalog[i].id);
        g_assert_nonnull(catalog[i].label);
        g_assert_cmpstr(catalog[i].id, !=, "");
        g_assert_cmpstr(catalog[i].label, !=, "");

        for (j = 0; j < catalog[i].n_models; j++) {
            g_assert_nonnull(catalog[i].models[j].id);
            g_assert_nonnull(catalog[i].models[j].label);
        }

        /*
         * A provider with no listed models must say its list is open,
         * or a client renders an empty combo and no way past it.
         */
        if (catalog[i].n_models == 0)
            g_assert_true(catalog[i].open_ended);
    }
}

/*
 * The two newest backends resolve to themselves, and their aliases
 * resolve to them.
 *
 * Named separately from the loop above because the loop only sees the
 * catalog's spellings.  Somebody configuring an agent by hand will
 * write what the binary is called -- `agy`, `cursor-agent` -- and an
 * alias that fell through to the warning would run their agent on
 * claude-code while blaming a typo they had not made.
 */
static void
test_the_new_cli_backends_and_their_aliases_resolve(void)
{
    g_assert_cmpstr(lc_provider_type_normalize("antigravity", "t"), ==,
                    "antigravity");
    g_assert_cmpstr(lc_provider_type_normalize("agy", "t"), ==,
                    "antigravity");

    g_assert_cmpstr(lc_provider_type_normalize("cursor", "t"), ==,
                    "cursor");
    g_assert_cmpstr(lc_provider_type_normalize("cursor-agent", "t"), ==,
                    "cursor");

    /* And they are both offered as something an agent can run on. */
    {
        const ClawtProviderInfo *ag =
            clawt_model_catalog_find_provider("antigravity");
        const ClawtProviderInfo *cu =
            clawt_model_catalog_find_provider("cursor");

        g_assert_nonnull(ag);
        g_assert_nonnull(cu);
        g_assert_true(ag->agent);
        g_assert_true(cu->agent);

        /*
         * And not as something the designer runs on. ai-glib's CLI
         * clients drop the tool list, and the designer is nothing but
         * tool calls -- offering one there produces a model that
         * answers in prose and a draft that never gets built.
         */
        g_assert_false(ag->tools);
        g_assert_false(cu->tools);
    }
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/model-catalog/agent-providers-are-libreclaws",
                    test_agent_providers_are_the_ones_libreclaw_runs);
    g_test_add_func("/model-catalog/new-cli-backends-resolve",
                    test_the_new_cli_backends_and_their_aliases_resolve);
    g_test_add_func("/model-catalog/http-providers-are-not-agents",
                    test_http_providers_are_not_offered_as_agents);
    g_test_add_func("/model-catalog/every-backend-is-offered",
                    test_every_libreclaw_backend_is_offered);
    g_test_add_func("/model-catalog/designer-default-takes-tools",
                    test_designer_default_can_take_tools);
    g_test_add_func("/model-catalog/agent-default-is-an-agent",
                    test_agent_default_can_be_an_agent);
    g_test_add_func("/model-catalog/entries-are-complete",
                    test_entries_are_complete);

    return g_test_run();
}
