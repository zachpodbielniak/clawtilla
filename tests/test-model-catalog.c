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
 * Where a swallowed log message lands.
 *
 * lc_provider_type_normalize() states its valid set only by warning
 * about a name outside it, so reading that set means reading the
 * warning.
 */
static GString *warning_sink = NULL;

static void
collect_warning(const gchar *domain, GLogLevelFlags level,
                const gchar *message, gpointer user_data)
{
    (void)domain;
    (void)level;
    (void)user_data;

    if (warning_sink != NULL && message != NULL)
        g_string_append(warning_sink, message);
}

/*
 * Normalise @name and hand back whatever libreclaw said while doing it.
 *
 * The handler and the fatal mask are installed and taken away around
 * this one call and nowhere wider, because a g_error() raised while our
 * own handler is installed is swallowed by it: the test aborts with 134
 * and prints nothing, which is a failing test that says nothing about
 * why.  Every assertion belongs outside this function.
 *
 * @out_said is (transfer full) and is never %NULL, so "did it warn" is a
 * length rather than a pointer test.
 */
static const gchar *
normalize_quietly(const gchar *name, gchar **out_said)
{
    const gchar *canonical;
    GLogFunc previous;
    GLogLevelFlags fatal;

    warning_sink = g_string_new(NULL);

    previous = g_log_set_default_handler(collect_warning, NULL);
    fatal = g_log_set_always_fatal(G_LOG_LEVEL_ERROR);

    /* Static storage in libreclaw, so it outlives the sink. */
    canonical = lc_provider_type_normalize(name, "model-catalog-test");

    g_log_set_always_fatal(fatal);
    g_log_set_default_handler(previous, NULL);

    *out_said = g_string_free(warning_sink, FALSE);
    warning_sink = NULL;

    return canonical;
}

/*
 * Every provider libreclaw can run an agent on is offered.
 *
 * grok-build -- xAI's grok CLI in headless mode -- was missing for a
 * while, so it could not be chosen at all even though libreclaw has
 * always known how to drive it.  Then antigravity and cursor arrived and
 * this test did not notice either, because it carried its own list of
 * four names: the file whose whole job is refusing a second copy of a
 * list had one, and a list nothing compares against is a list that has
 * already drifted.
 *
 * libreclaw exports no enumeration of its backends -- no
 * `lc_provider_type_count()`/`_nth()`, no GEnum, nothing on
 * lc-client-factory either.  Its one machine-readable statement of the
 * set is the "Valid: ..." tail of the warning
 * lc_provider_type_normalize() emits for a name it does not know, and
 * that is the same sentence an operator with a typo'd provider is shown.
 * So the set is read from there rather than written down here again, and
 * each name is put back through libreclaw's own normalize() to get the
 * canonical spelling -- the message deliberately lists aliases
 * (`claude-code-tmux`) beside canonical names.
 *
 * What this cannot see: a backend lc_client_factory_new() builds and the
 * warning forgets to name.  That would be a defect in libreclaw's own
 * message, worth failing over on its own, and this test would report
 * nothing about it.
 */
static void
test_every_libreclaw_backend_is_offered(void)
{
    g_autofree gchar *said = NULL;
    g_auto(GStrv) names = NULL;
    g_autofree gchar *valid = NULL;
    const gchar *at;
    const gchar *end;
    guint i;
    guint checked = 0;

    (void)normalize_quietly("clawtilla-not-a-backend", &said);

    at = strstr(said, "Valid: ");

    if (at == NULL)
        g_error("libreclaw's unknown-provider warning no longer names its "
                "valid set, so nothing here can enumerate the backends. "
                "Give lc-provider-resolve.h a real enumeration and walk "
                "that rather than restoring a list in this file. It said: "
                "%s", said);

    at += strlen("Valid: ");
    end = strchr(at, '.');
    valid = g_strndup(at, end != NULL ? (gsize)(end - at) : strlen(at));

    names = g_strsplit(valid, ",", -1);

    for (i = 0; names[i] != NULL; i++) {
        g_autofree gchar *again = NULL;
        const gchar *canonical;
        const ClawtProviderInfo *provider;
        gchar *name = g_strstrip(names[i]);

        if (*name == '\0')
            continue;

        canonical = normalize_quietly(name, &again);

        /*
         * A listed name normalize() does not accept means libreclaw's
         * message and its code disagree about the set -- and every
         * assertion below would then be about claude-code.
         */
        if (*again != '\0')
            g_error("libreclaw's warning lists '%s' as valid and its own "
                    "normalize() does not accept it", name);

        provider = clawt_model_catalog_find_provider(canonical);

        if (provider == NULL)
            g_error("no catalogue entry for libreclaw backend '%s' "
                    "(listed as '%s')", canonical, name);

        if (!provider->agent)
            g_error("catalogue entry '%s' is not offered as something an "
                    "agent runs on, and libreclaw runs agents on it",
                    canonical);

        checked++;
    }

    /*
     * A parse that produced nothing looks exactly like a set that is
     * entirely offered.
     */
    g_assert_cmpuint(checked, >, 0);
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
